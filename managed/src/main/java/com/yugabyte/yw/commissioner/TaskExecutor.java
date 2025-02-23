// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.commissioner;

import static com.google.common.base.Preconditions.checkArgument;
import static com.google.common.base.Preconditions.checkNotNull;
import static com.yugabyte.yw.models.helpers.CommonUtils.getDurationSeconds;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.api.client.util.Throwables;
import com.google.common.annotations.VisibleForTesting;
import com.google.common.collect.Sets;
import com.yugabyte.yw.commissioner.ITask.Abortable;
import com.yugabyte.yw.commissioner.ITask.Retryable;
import com.yugabyte.yw.commissioner.UserTaskDetails.SubTaskGroupType;
import com.yugabyte.yw.common.DrainableMap;
import com.yugabyte.yw.common.Util;
import com.yugabyte.yw.common.ha.PlatformReplicationManager;
import com.yugabyte.yw.common.password.RedactingService;
import com.yugabyte.yw.forms.ITaskParams;
import com.yugabyte.yw.models.CustomerTask;
import com.yugabyte.yw.models.ScheduleTask;
import com.yugabyte.yw.models.TaskInfo;
import com.yugabyte.yw.models.TaskInfo.State;
import com.yugabyte.yw.models.helpers.CommonUtils;
import com.yugabyte.yw.models.helpers.KnownAlertLabels;
import com.yugabyte.yw.models.helpers.TaskType;
import io.prometheus.client.CollectorRegistry;
import io.prometheus.client.Summary;
import java.time.Duration;
import java.time.Instant;
import java.util.Collections;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Queue;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.stream.Collectors;
import javax.inject.Inject;
import javax.inject.Singleton;
import lombok.extern.slf4j.Slf4j;
import org.apache.commons.lang3.StringUtils;
import play.api.Play;
import play.inject.ApplicationLifecycle;

/**
 * TaskExecutor is the executor service for tasks and their subtasks. It is very similar to the
 * current SubTaskGroupQueue and SubTaskGroup.
 *
 * <p>A task is submitted by first creating a RunnableTask.
 *
 * <pre>
 * RunnableTask runnableTask = taskExecutor.createRunnableTask(taskType, taskParams);
 * UUID taskUUID = taskExecutor.submit(runnableTask, executor);
 * </pre>
 *
 * The RunnableTask instance is first retrieved in the run() method of the task.
 *
 * <pre>
 * RunnableTask runnableTask = taskExecutor.getRunnableTask(UUID).
 * </pre>
 *
 * This is similar to the current implementation of SubTaskGroupQueue queue = new
 * SubTaskGroupQueue(UUID).
 *
 * <p>The subtasks are added by first adding them to their groups and followed by adding the groups
 * to the RunnableTask instance.
 *
 * <pre>
 * void createProvisionNodes(List<Node> nodes, SubTaskGroupType groupType) {
 *    SubTasksGroup group = taskExecutor.createSubTaskGroup("provision-nodes",
 *                                  groupType);
 *    for (Node node : nodes) {
 *        // Create the subtask instance and initialize.
 *        ITask subTask = createAndInitSubTask(node);
 *        // Add the concurrent subtasks to the group.
 *        group.addSubTask(subTask);
 *    }
 *    runnableTask.addSubTaskGroup(group);
 * }
 * </pre>
 *
 * After all the subtasks are added, runSubTasks() is invoked in the run() method of the task. e.g
 *
 * <pre>
 * // Run method of task
 * void run() {
 *    // Same as the current queue = new SubTaskGroupQueue(UUID);
 *    runnableTask = taskExecutor.getRunnableTask(UUID).
 *    // Creates subtask group which is then added to runnableTask.
 *    createTasks1(nodes);
 *    createTasks2(nodes);
 *    createTasks3(nodes);
 *    // Same as the current queue.run();
 *    runnableTask.runSubTasks();
 * }
 * </pre>
 */
@Singleton
@Slf4j
public class TaskExecutor {

  // This is a map from the task types to the classes.
  private static final Map<TaskType, Class<? extends ITask>> TASK_TYPE_TO_CLASS_MAP;

  // Task futures are waited for this long before checking abort status.
  private static final long TASK_SPIN_WAIT_INTERVAL_MS = 2000;

  // Default wait timeout for subtasks to complete since the abort call.
  private final Duration defaultAbortTaskTimeout = Duration.ofSeconds(60);

  // ExecutorService provider for subtasks if explicit ExecutorService
  // is set for the subtasks in a task.
  private final ExecutorServiceProvider executorServiceProvider;

  // A map from task UUID to its RunnableTask while it is running.
  private final DrainableMap<UUID, RunnableTask> runnableTasks = new DrainableMap<>();

  // A utility for Platform HA.
  private final PlatformReplicationManager replicationManager;

  private final AtomicBoolean isShutdown = new AtomicBoolean();

  private final String taskOwner;

  // Skip or perform abortable check for subtasks.
  private final boolean skipSubTaskAbortableCheck;

  private static final String COMMISSIONER_TASK_WAITING_SEC_METRIC =
      "ybp_commissioner_task_waiting_sec";

  private static final String COMMISSIONER_TASK_EXECUTION_SEC_METRIC =
      "ybp_commissioner_task_execution_sec";

  private static final Summary COMMISSIONER_TASK_WAITING_SEC =
      buildSummary(
          COMMISSIONER_TASK_WAITING_SEC_METRIC,
          "Duration between task creation and execution",
          KnownAlertLabels.TASK_TYPE.labelName());

  private static final Summary COMMISSIONER_TASK_EXECUTION_SEC =
      buildSummary(
          COMMISSIONER_TASK_EXECUTION_SEC_METRIC,
          "Duration of task execution",
          KnownAlertLabels.TASK_TYPE.labelName(),
          KnownAlertLabels.RESULT.labelName());

  static {
    // Initialize the map which holds the task types to their task class.
    Map<TaskType, Class<? extends ITask>> typeMap = new HashMap<>();

    for (TaskType taskType : TaskType.filteredValues()) {
      String className = "com.yugabyte.yw.commissioner.tasks." + taskType.toString();
      Class<? extends ITask> taskClass;
      try {
        taskClass = Class.forName(className).asSubclass(ITask.class);
        typeMap.put(taskType, taskClass);
        log.debug("Found task: {}", className);
      } catch (ClassNotFoundException e) {
        log.error("Could not find task for task type " + taskType, e);
      }
    }
    TASK_TYPE_TO_CLASS_MAP = Collections.unmodifiableMap(typeMap);
    log.debug("Done loading tasks.");
  }

  private static Summary buildSummary(String name, String description, String... labelNames) {
    return Summary.build(name, description)
        .quantile(0.5, 0.05)
        .quantile(0.9, 0.01)
        .maxAgeSeconds(TimeUnit.HOURS.toSeconds(1))
        .labelNames(labelNames)
        .register(CollectorRegistry.defaultRegistry);
  }

  // This writes the waiting time metric.
  private static void writeTaskWaitMetric(
      TaskType taskType, Instant scheduledTime, Instant startTime) {
    COMMISSIONER_TASK_WAITING_SEC
        .labels(taskType.name())
        .observe(getDurationSeconds(scheduledTime, startTime));
  }

  // This writes the execution time metric.
  private static void writeTaskStateMetric(
      TaskType taskType, Instant startTime, Instant endTime, State state) {
    COMMISSIONER_TASK_EXECUTION_SEC
        .labels(taskType.name(), state.name())
        .observe(getDurationSeconds(startTime, endTime));
  }

  static Class<? extends ITask> getTaskClass(TaskType taskType) {
    checkNotNull(taskType, "Task type must be non-null");
    return TASK_TYPE_TO_CLASS_MAP.get(taskType);
  }

  // It looks for the annotation starting from the current class to its super classes until it
  // finds. If it is not found, it returns false, else the value of enabled is returned. It is
  // possible to override an annotation already defined in the superclass.
  static boolean isTaskAbortable(Class<? extends ITask> taskClass) {
    checkNotNull(taskClass, "Task class must be non-null");
    Optional<Abortable> optional = CommonUtils.isAnnotatedWith(taskClass, Abortable.class);
    if (optional.isPresent()) {
      return optional.get().enabled();
    }
    return false;
  }

  // It looks for the annotation starting from the current class to its super classes until it
  // finds. If it is not found, it returns false, else the value of enabled is returned. It is
  // possible to override an annotation already defined in the superclass.
  static boolean isTaskRetryable(Class<? extends ITask> taskClass) {
    checkNotNull(taskClass, "Task class must be non-null");
    Optional<Retryable> optional = CommonUtils.isAnnotatedWith(taskClass, Retryable.class);
    if (optional.isPresent()) {
      return optional.get().enabled();
    }
    return false;
  }

  @Inject
  public TaskExecutor(
      ApplicationLifecycle lifecycle,
      ExecutorServiceProvider executorServiceProvider,
      PlatformReplicationManager replicationManager) {
    this.executorServiceProvider = executorServiceProvider;
    this.replicationManager = replicationManager;
    this.taskOwner = Util.getHostname();
    this.skipSubTaskAbortableCheck = true;
    lifecycle.addStopHook(
        () ->
            CompletableFuture.supplyAsync(() -> TaskExecutor.this.shutdown(Duration.ofMinutes(5))));
  }

  // Shuts down the task executor.
  // It assumes that the executor services will
  // also be shutdown gracefully.
  public boolean shutdown(Duration timeout) {
    if (isShutdown.compareAndSet(false, true)) {
      log.info("TaskExecutor is shutting down");
      runnableTasks.sealMap();
      Instant abortTime = Instant.now();
      synchronized (runnableTasks) {
        runnableTasks.forEach(
            (uuid, runnable) -> {
              runnable.setAbortTime(abortTime);
            });
      }
    }
    try {
      // Wait for all the RunnableTask to be done.
      // A task in runnableTasks map is removed when it is cancelled due to executor shutdown or
      // when it is completed.
      return runnableTasks.waitForEmpty(timeout);
    } catch (InterruptedException e) {
      log.error("Wait for task completion interrupted", e);
    }
    return false;
  }

  private void checkTaskExecutorState() {
    if (isShutdown.get()) {
      throw new IllegalStateException("TaskExecutor is shutting down");
    }
  }

  /**
   * Creates a RunnableTask instance for a task with the given parameters.
   *
   * @param taskType the task type.
   * @param taskParams the task parameters.
   * @return
   */
  public RunnableTask createRunnableTask(TaskType taskType, ITaskParams taskParams) {
    checkNotNull(taskType, "Task type must be set");
    checkNotNull(taskParams, "Task params must be set");
    ITask task = Play.current().injector().instanceOf(TASK_TYPE_TO_CLASS_MAP.get(taskType));
    task.initialize(taskParams);
    return createRunnableTask(task);
  }

  /**
   * Creates a RunnableTask instance for the given task.
   *
   * @param task the task.
   * @return
   */
  public RunnableTask createRunnableTask(ITask task) {
    checkNotNull(task, "Task must be set");
    TaskInfo taskInfo = createTaskInfo(task);
    taskInfo.setPosition(-1);
    taskInfo.save();
    return new RunnableTask(task, taskInfo);
  }

  /**
   * Submits a RunnableTask to this TaskExecutor with the given ExecutorService for execution. If
   * the task has subtasks, they are submitted to this RunnableTask in its run() method.
   *
   * @param runnableTask the RunnableTask instance.
   * @param taskExecutorService the ExecutorService for this task.
   * @return
   */
  public UUID submit(RunnableTask runnableTask, ExecutorService taskExecutorService) {
    checkTaskExecutorState();
    checkNotNull(runnableTask, "Task runnable must not be null");
    checkNotNull(taskExecutorService, "Task executor service must not be null");
    UUID taskUUID = runnableTask.getTaskUUID();
    runnableTasks.put(taskUUID, runnableTask);
    try {
      runnableTask.updateScheduledTime();
      runnableTask.future = taskExecutorService.submit(runnableTask);
    } catch (Exception e) {
      // Update task state on submission failure.
      runnableTasks.remove(taskUUID);
      runnableTask.updateTaskDetailsOnError(TaskInfo.State.Failure, e);
    }
    return taskUUID;
  }

  // This waits for the parent task to complete indefinitely.
  public void waitForTask(UUID taskUUID) {
    waitForTask(taskUUID, null);
  }

  // This waits for the parent task to complete within the timeout.
  public void waitForTask(UUID taskUUID, Duration timeout) {
    Optional<RunnableTask> optional = maybeGetRunnableTask(taskUUID);
    if (!optional.isPresent()) {
      return;
    }
    RunnableTask runnableTask = optional.get();
    try {
      if (timeout == null || timeout.isZero()) {
        runnableTask.future.get();
      } else {
        runnableTask.future.get(timeout.toMillis(), TimeUnit.MILLISECONDS);
      }
    } catch (ExecutionException e) {
      Throwables.propagate(e.getCause());
    } catch (Exception e) {
      Throwables.propagate(e);
    }
  }

  @VisibleForTesting
  boolean isTaskRunning(UUID taskUUID) {
    return runnableTasks.containsKey(taskUUID);
  }

  /**
   * It signals to abort a task if it is already running. When a running task is aborted
   * asynchronously, the task state changes to 'Aborted'. It does not validate if the UUID
   * represents a valid task and returns an empty Optional if the task is either not running or not
   * found.
   *
   * @param taskUUID UUID of the task.
   * @return returns an optional TaskInfo that is present if the task is already found running.
   */
  public Optional<TaskInfo> abort(UUID taskUUID) {
    log.info("Aborting task {}", taskUUID);
    Optional<RunnableTask> optional = maybeGetRunnableTask(taskUUID);
    if (!optional.isPresent()) {
      log.info("Task {} is not found. It is either completed or non-existing", taskUUID);
      return Optional.empty();
    }
    RunnableTask runnableTask = optional.get();
    ITask task = runnableTask.task;
    if (!isTaskAbortable(task.getClass())) {
      throw new RuntimeException("Task " + task.getName() + " is not abortable");
    }
    // Signal abort to the task.
    if (runnableTask.getAbortTime() == null) {
      // This is not atomic but it is ok.
      runnableTask.setAbortTime(Instant.now());
    }
    // Update the task state in the memory and DB.
    runnableTask.compareAndSetTaskState(
        Sets.immutableEnumSet(State.Initializing, State.Created, State.Running), State.Abort);
    return Optional.of(runnableTask.taskInfo);
  }

  /**
   * Creates a SubTaskGroup with the given name.
   *
   * @param name the name of the group.
   * @return SubTaskGroup
   */
  public SubTaskGroup createSubTaskGroup(String name) {
    return createSubTaskGroup(name, SubTaskGroupType.Invalid, false);
  }

  /**
   * Creates a SubTaskGroup to which subtasks can be added for concurrent execution.
   *
   * @param name the name of the group.
   * @param subTaskGroupType the type/phase of the subtasks group.
   * @param ignoreErrors ignore individual subtask error until the all the subtasks in the group are
   *     executed if it is set.
   * @return SubTaskGroup
   */
  public SubTaskGroup createSubTaskGroup(
      String name, SubTaskGroupType subTaskGroupType, boolean ignoreErrors) {
    return new SubTaskGroup(name, subTaskGroupType, ignoreErrors);
  }

  @VisibleForTesting
  TaskInfo createTaskInfo(ITask task) {
    TaskType taskType = TaskType.valueOf(task.getClass().getSimpleName());
    // Create a new task info object.
    TaskInfo taskInfo = new TaskInfo(taskType);
    // Set the task details.
    taskInfo.setTaskDetails(RedactingService.filterSecretFields(task.getTaskDetails()));
    // Set the owner info.
    taskInfo.setOwner(taskOwner);
    return taskInfo;
  }

  /**
   * Returns the current RunnableTask instance for the given task UUID. Subtasks can be submitted to
   * this instance and run. It throws IllegalStateException if the task is not present.
   *
   * @param taskUUID the task UUID.
   * @return
   */
  public RunnableTask getRunnableTask(UUID taskUUID) {
    Optional<RunnableTask> optional = maybeGetRunnableTask(taskUUID);
    return optional.orElseThrow(
        () -> new IllegalStateException(String.format("Task(%s) is not present", taskUUID)));
  }

  // Optionally returns the task runnable with the given task UUID.
  private Optional<RunnableTask> maybeGetRunnableTask(UUID taskUUID) {
    return Optional.ofNullable(runnableTasks.get(taskUUID));
  }

  /**
   * Listener to get called when a task is executed. This is useful for testing to throw exception
   * before a task is executed. Throwing CancellationException in beforeTask aborts the task.
   */
  @FunctionalInterface
  public interface TaskExecutionListener {
    default void beforeTask(TaskInfo taskInfo) {}

    void afterTask(TaskInfo taskInfo, Throwable t);
  }

  /**
   * A placeholder for a group of subtasks to be executed concurrently later. Subtasks requiring
   * concurrent executions are added to a group which is then added to the RunnableTask instance for
   * the task.
   */
  public class SubTaskGroup {
    private final Set<RunnableSubTask> subTasks =
        Collections.newSetFromMap(new ConcurrentHashMap<RunnableSubTask, Boolean>());
    private final String name;
    private final boolean ignoreErrors;
    private final AtomicInteger numTasksCompleted;

    // Parent task runnable to which this group belongs.
    private volatile RunnableTask runnableTask;
    // Optional executor service for the subtasks.
    private ExecutorService executorService;
    private SubTaskGroupType subTaskGroupType = SubTaskGroupType.Invalid;

    // It is instantiated internally.
    private SubTaskGroup(String name, SubTaskGroupType subTaskGroupType, boolean ignoreErrors) {
      this.name = name;
      this.ignoreErrors = ignoreErrors;
      this.numTasksCompleted = new AtomicInteger();
    }

    /**
     * Adds a subtask to this SubTaskGroup. It adds the subtask in memory and is not run yet. When
     * the this SubTaskGroup is added to the RunnableTask, the subtasks are persisted.
     *
     * @param subTask the subtask.
     */
    public void addSubTask(ITask subTask) {
      checkNotNull(subTask, "Subtask must be non-null");
      int subTaskCount = getSubTaskCount();
      log.info("Adding task #{}: {}", subTaskCount, subTask.getName());
      if (log.isDebugEnabled()) {
        JsonNode redactedTask = RedactingService.filterSecretFields(subTask.getTaskDetails());
        log.debug(
            "Details for task #{}: {} details= {}", subTaskCount, subTask.getName(), redactedTask);
      }
      TaskInfo taskInfo = createTaskInfo(subTask);
      taskInfo.setSubTaskGroupType(subTaskGroupType);
      subTasks.add(new RunnableSubTask(subTask, taskInfo));
    }

    /**
     * Sets an optional ExecutorService for the subtasks in this group. If it is not set, an
     * ExecutorService is created by ExecutorServiceProvider for the parent task type.
     *
     * @param executorService the ExecutorService.
     */
    public void setSubTaskExecutor(ExecutorService executorService) {
      this.executorService = executorService;
    }

    public String getName() {
      return name;
    }

    /**
     * Returns the optional ExecutorService for the subtasks in this group.
     *
     * @return
     */
    private ExecutorService getSubTaskExecutorService() {
      return executorService;
    }

    private void setRunnableTaskContext(RunnableTask runnableTask, int position) {
      this.runnableTask = runnableTask;
      for (RunnableSubTask runnable : subTasks) {
        runnable.setRunnableTaskContext(runnableTask, position);
      }
    }

    // Submits the subtasks in the group to the ExecutorService.
    private void submitSubTasks() {
      for (RunnableSubTask runnable : subTasks) {
        runnable.executeWith(executorService);
      }
    }

    // Removes the completed subtask from the iterator.
    private void removeCompletedSubTask(
        Iterator<RunnableSubTask> taskIterator,
        RunnableSubTask runnableSubTask,
        Throwable throwable) {
      if (throwable != null) {
        log.error("Error occurred in subtask " + runnableSubTask.taskInfo, throwable);
      }
      taskIterator.remove();
      numTasksCompleted.incrementAndGet();
      runnableSubTask.publishAfterTask(throwable);
    }

    // Wait for all the subtasks to complete. In this method, the state updates on
    // exceptions are done for tasks which are not yet running and exception occurs.
    private void waitForSubTasks() {
      UUID parentTaskUUID = runnableTask.getTaskUUID();
      Instant waitStartTime = Instant.now();
      List<RunnableSubTask> runnableSubTasks =
          this.subTasks.stream().filter(t -> t.future != null).collect(Collectors.toList());

      Throwable anyEx = null;
      while (runnableSubTasks.size() > 0) {
        Iterator<RunnableSubTask> iter = runnableSubTasks.iterator();

        // Start round-robin check on the task completion to give fair share to each subtask.
        while (iter.hasNext()) {
          RunnableSubTask runnableSubTask = iter.next();
          Future<?> future = runnableSubTask.future;

          try {
            future.get(TASK_SPIN_WAIT_INTERVAL_MS, TimeUnit.MILLISECONDS);
            removeCompletedSubTask(iter, runnableSubTask, null);
          } catch (ExecutionException e) {
            // Ignore state update because this exception is thrown
            // during the task execution and is already taken care
            // by RunnableSubTask.
            anyEx = e.getCause();
            removeCompletedSubTask(iter, runnableSubTask, anyEx);
          } catch (TimeoutException e) {
            // The exception is ignored if the elapsed time has not surpassed
            // the time limit.
            Duration elapsed = Duration.between(waitStartTime, Instant.now());
            if (log.isTraceEnabled()) {
              log.trace("Task {} has taken {}ms", parentTaskUUID, elapsed.toMillis());
            }
            Duration timeout = runnableSubTask.getTimeLimit();
            Instant abortTime = runnableTask.getAbortTime();
            // If the subtask execution takes long, it is interrupted.
            if (!timeout.isZero() && elapsed.compareTo(timeout) > 0) {
              anyEx = e;
              future.cancel(true);
              // Report failure to the parent task.
              // Update the subtask state to aborted if the execution timed out.
              runnableSubTask.updateTaskDetailsOnError(TaskInfo.State.Aborted, e);
              removeCompletedSubTask(iter, runnableSubTask, e);
            } else if (abortTime != null
                && Duration.between(abortTime, Instant.now()).compareTo(defaultAbortTaskTimeout) > 0
                && (skipSubTaskAbortableCheck
                    || isTaskAbortable(runnableSubTask.task.getClass()))) {
              future.cancel(true);
              // Report aborted to the parent task.
              // Update the subtask state to aborted if the execution timed out.
              anyEx = new CancellationException(e.getMessage());
              runnableSubTask.updateTaskDetailsOnError(TaskInfo.State.Aborted, anyEx);
              removeCompletedSubTask(iter, runnableSubTask, anyEx);
            }
          } catch (CancellationException e) {
            anyEx = e;
            runnableSubTask.updateTaskDetailsOnError(TaskInfo.State.Aborted, e);
            removeCompletedSubTask(iter, runnableSubTask, e);
          } catch (InterruptedException e) {
            anyEx = new CancellationException(e.getMessage());
            runnableSubTask.updateTaskDetailsOnError(TaskInfo.State.Aborted, anyEx);
            removeCompletedSubTask(iter, runnableSubTask, anyEx);
          } catch (Exception e) {
            anyEx = e;
            runnableSubTask.updateTaskDetailsOnError(TaskInfo.State.Failure, e);
            removeCompletedSubTask(iter, runnableSubTask, e);
          }
        }
      }
      if (anyEx != null) {
        Throwables.propagate(anyEx);
      }
    }

    /**
     * Sets the SubTaskGroupType for this SubTaskGroup.
     *
     * @param subTaskGroupType the SubTaskGroupType.
     */
    public void setSubTaskGroupType(SubTaskGroupType subTaskGroupType) {
      if (subTaskGroupType == null) {
        return;
      }
      log.info("Setting subtask({}} group type to {}", name, subTaskGroupType);
      this.subTaskGroupType = subTaskGroupType;
      for (RunnableSubTask runnable : subTasks) {
        runnable.setSubTaskGroupType(subTaskGroupType);
      }
    }

    public int getSubTaskCount() {
      return subTasks.size();
    }

    public int getTasksCompletedCount() {
      return numTasksCompleted.get();
    }

    @Override
    public String toString() {
      return name
          + " : completed "
          + getTasksCompletedCount()
          + " out of "
          + getSubTaskCount()
          + " tasks.";
    }
  }

  /**
   * Abstract implementation of a task runnable which handles the state update after the task has
   * started running. Synchronization is on the this object for taskInfo.
   */
  public abstract class AbstractRunnableTask implements Runnable {
    final ITask task;
    final TaskInfo taskInfo;
    // Timeout limit for this task.
    final Duration timeLimit;

    Instant taskScheduledTime;
    Instant taskStartTime;
    Instant taskCompletionTime;

    // Future of the task that is set after it is submitted to the ExecutorService.
    Future<?> future = null;

    protected AbstractRunnableTask(ITask task, TaskInfo taskInfo) {
      this.task = task;
      this.taskInfo = taskInfo;
      this.taskScheduledTime = Instant.now();

      Duration duration = Duration.ZERO;
      JsonNode jsonNode = taskInfo.getTaskDetails();
      if (jsonNode != null && !jsonNode.isNull()) {
        JsonNode timeLimitJsonNode = jsonNode.get("timeLimitMins");
        if (timeLimitJsonNode != null && !timeLimitJsonNode.isNull()) {
          long timeLimitMins = Long.parseLong(timeLimitJsonNode.asText());
          duration = Duration.ofMinutes(timeLimitMins);
        }
      }
      timeLimit = duration;
    }

    // State and error message updates to tasks are done in this method instead of waitForSubTasks
    // because nobody is waiting for the parent task.
    @Override
    public void run() {
      Throwable t = null;
      TaskType taskType = taskInfo.getTaskType();
      taskStartTime = Instant.now();
      try {
        writeTaskWaitMetric(taskType, taskScheduledTime, taskStartTime);
        if (getAbortTime() != null) {
          throw new CancellationException("Task " + task.getName() + " is aborted");
        }
        publishBeforeTask();
        setTaskState(TaskInfo.State.Running);
        log.debug("Invoking run() of task {}", task.getName());
        task.run();
        setTaskState(TaskInfo.State.Success);
      } catch (CancellationException e) {
        t = e;
        updateTaskDetailsOnError(TaskInfo.State.Aborted, e);
        throw e;
      } catch (Exception e) {
        t = e;
        updateTaskDetailsOnError(TaskInfo.State.Failure, e);
        Throwables.propagate(e);
      } finally {
        taskCompletionTime = Instant.now();
        writeTaskStateMetric(taskType, taskStartTime, taskCompletionTime, getTaskState());
        publishAfterTask(t);
        task.terminate();
      }
    }

    public synchronized boolean isTaskRunning() {
      return taskInfo.getTaskState() == TaskInfo.State.Running;
    }

    public synchronized boolean hasTaskSucceeded() {
      return taskInfo.getTaskState() == TaskInfo.State.Success;
    }

    public synchronized boolean hasTaskFailed() {
      return taskInfo.getTaskState() == TaskInfo.State.Failure;
    }

    @Override
    public String toString() {
      return "task-info {" + taskInfo.toString() + "}" + ", task {" + task.getName() + "}";
    }

    public UUID getTaskUUID() {
      return taskInfo.getTaskUUID();
    }

    // This is invoked from tasks to save the updated task details generally in transaction with
    // other DB updates.
    public synchronized void setTaskDetails(JsonNode taskDetails) {
      taskInfo.refresh();
      taskInfo.setTaskDetails(taskDetails);
      taskInfo.update();
    }

    protected abstract Instant getAbortTime();

    protected abstract TaskExecutionListener getTaskExecutionLister();

    Duration getTimeLimit() {
      return timeLimit;
    }

    void updateScheduledTime() {
      taskScheduledTime = Instant.now();
    }

    TaskType getTaskType() {
      return taskInfo.getTaskType();
    }

    synchronized void setPosition(int position) {
      taskInfo.setPosition(position);
    }

    synchronized TaskInfo.State getTaskState() {
      return taskInfo.getTaskState();
    }

    synchronized void setTaskState(TaskInfo.State state) {
      taskInfo.setTaskState(state);
      taskInfo.update();
    }

    synchronized boolean compareAndSetTaskState(TaskInfo.State expected, TaskInfo.State state) {
      return compareAndSetTaskState(Sets.immutableEnumSet(expected), state);
    }

    synchronized boolean compareAndSetTaskState(
        Set<TaskInfo.State> expectedStates, TaskInfo.State state) {
      TaskInfo.State currentState = taskInfo.getTaskState();
      if (expectedStates.contains(currentState)) {
        setTaskState(state);
        return true;
      }
      return false;
    }

    synchronized void updateTaskDetailsOnError(TaskInfo.State state, Throwable t) {
      checkNotNull(t);
      checkArgument(
          TaskInfo.ERROR_STATES.contains(state),
          "Task state must be one of " + TaskInfo.ERROR_STATES);
      JsonNode taskDetails = taskInfo.getTaskDetails();
      String errorString =
          "Failed to execute task "
              + StringUtils.abbreviate(taskDetails.toString(), 500)
              + ", hit error:\n\n"
              + StringUtils.abbreviateMiddle(t.getMessage(), "...", 3000)
              + ".";
      log.error(
          "Failed to execute task type {} UUID {} details {}, hit error.",
          taskInfo.getTaskType(),
          taskInfo.getTaskUUID(),
          taskDetails,
          t);

      ObjectNode details = taskDetails.deepCopy();
      details.put("errorString", errorString);
      taskInfo.refresh();
      taskInfo.setTaskState(state);
      taskInfo.setTaskDetails(details);
      taskInfo.update();
    }

    void publishBeforeTask() {
      TaskExecutionListener taskExecutionListener = getTaskExecutionLister();
      if (taskExecutionListener != null) {
        taskExecutionListener.beforeTask(taskInfo);
      }
    }

    void publishAfterTask(Throwable t) {
      TaskExecutionListener taskExecutionListener = getTaskExecutionLister();
      if (taskExecutionListener != null) {
        taskExecutionListener.afterTask(taskInfo, t);
      }
    }
  }

  /** Task runnable */
  public class RunnableTask extends AbstractRunnableTask {
    // Subtask groups to hold subtasks.
    private final Queue<SubTaskGroup> subTaskGroups = new ConcurrentLinkedQueue<>();
    // Current execution position of subtasks.
    private int subTaskPosition = 0;
    private TaskExecutionListener taskExecutionListener;
    // Time when the abort is set.
    private volatile Instant abortTime;

    RunnableTask(ITask task, TaskInfo taskInfo) {
      super(task, taskInfo);
    }

    /**
     * Sets a TaskExecutionListener instance to get callbacks on before and after each task
     * execution.
     *
     * @param taskExecutionListener the TaskExecutionListener instance.
     */
    public void setTaskExecutionListener(TaskExecutionListener taskExecutionListener) {
      this.taskExecutionListener = taskExecutionListener;
    }

    /** Invoked by the ExecutorService. Do not invoke this directly. */
    @Override
    public void run() {
      UUID taskUUID = taskInfo.getTaskUUID();
      try {
        task.setUserTaskUUID(taskUUID);
        super.run();
      } catch (Exception e) {
        Throwables.propagate(e);
      } finally {
        // Remove the task.
        runnableTasks.remove(taskUUID);
        // Update the customer task to a completed state.
        CustomerTask customerTask = CustomerTask.findByTaskUUID(taskUUID);
        if (customerTask != null) {
          customerTask.markAsCompleted();
        }

        // In case, it is a scheduled task, update state of the task.
        ScheduleTask scheduleTask = ScheduleTask.fetchByTaskUUID(taskUUID);
        if (scheduleTask != null) {
          scheduleTask.setCompletedTime();
        }
        // Run a one-off Platform HA sync every time a task finishes.
        replicationManager.oneOffSync();
      }
    }

    /**
     * Clears the already added subtask groups so that they are not run when the RunnableTask is
     * re-run.
     */
    public void reset() {
      subTaskGroups.clear();
      subTaskPosition = 0;
    }

    @Override
    protected Instant getAbortTime() {
      return abortTime;
    }

    private void setAbortTime(Instant abortTime) {
      this.abortTime = abortTime;
    }

    @Override
    protected TaskExecutionListener getTaskExecutionLister() {
      return taskExecutionListener;
    }

    public synchronized void doHeartbeat() {
      log.trace("Heartbeating task {}", getTaskUUID());
      TaskInfo taskInfo = TaskInfo.getOrBadRequest(getTaskUUID());
      taskInfo.markAsDirty();
      taskInfo.update();
    }

    /**
     * Adds the SubTaskGroup instance containing the subtasks which are to be executed concurrently.
     *
     * @param subTaskGroup the subtask group of subtasks to be executed concurrently.
     */
    public void addSubTaskGroup(SubTaskGroup subTaskGroup) {
      log.info("Adding SubTaskGroup #{}: {}", subTaskGroups.size(), subTaskGroup.name);
      subTaskGroup.setRunnableTaskContext(this, subTaskPosition);
      subTaskGroups.add(subTaskGroup);
      subTaskPosition++;
    }

    public void addSubTaskGroup(SubTaskGroup subTaskGroup, int position) {
      subTaskPosition = position;
      addSubTaskGroup(subTaskGroup);
    }

    /**
     * Starts execution of the subtasks in the groups for this runnable task and waits for
     * completion. This method is invoked inside the run() method of the task e.g CreateUniverse to
     * start execution of the subtasks.
     */
    public void runSubTasks() {
      RuntimeException anyRe = null;
      for (SubTaskGroup subTaskGroup : subTaskGroups) {
        if (subTaskGroup.getSubTaskCount() == 0) {
          // TODO Some groups are added without any subtasks in a task like
          // CreateKubernetesUniverse.
          // It needs to be fixed first before this can prevent empty groups from getting added.
          continue;
        }
        ExecutorService executorService = subTaskGroup.getSubTaskExecutorService();
        if (executorService == null) {
          executorService = executorServiceProvider.getExecutorServiceFor(getTaskType());
          subTaskGroup.setSubTaskExecutor(executorService);
        }
        checkNotNull(executorService, "ExecutorService must be set");
        try {
          try {
            // This can throw rare exception on task submission error.
            subTaskGroup.submitSubTasks();
          } finally {
            // TODO Does it make sense to abort the task?
            // There can be conflicts between aborted and failed task states.
            // Wait for already submitted subtasks.
            subTaskGroup.waitForSubTasks();
          }
        } catch (CancellationException e) {
          throw new CancellationException(subTaskGroup.toString() + " is cancelled.");
        } catch (RuntimeException e) {
          if (subTaskGroup.ignoreErrors) {
            log.error("Ignoring error for " + subTaskGroup.toString(), e);
          } else {
            // Postpone throwing this error later when all the subgroups are done.
            throw new RuntimeException(subTaskGroup.toString() + " failed.");
          }
          anyRe = e;
        }
      }
      if (anyRe != null) {
        throw new RuntimeException("One or more SubTaskGroups failed while running.");
      }
    }
  }

  /** Runnable task for subtasks in a task. */
  public class RunnableSubTask extends AbstractRunnableTask {
    private RunnableTask parentRunnableTask;

    RunnableSubTask(ITask task, TaskInfo taskInfo) {
      super(task, taskInfo);
    }

    private void executeWith(ExecutorService executorService) {
      try {
        updateScheduledTime();
        future = executorService.submit(this);
      } catch (RuntimeException e) {
        // Subtask submission failed.
        updateTaskDetailsOnError(TaskInfo.State.Failure, e);
        publishAfterTask(e);
        throw e;
      }
    }

    @Override
    public void run() {
      super.run();
    }

    @Override
    protected synchronized Instant getAbortTime() {
      return parentRunnableTask == null ? null : parentRunnableTask.getAbortTime();
    }

    @Override
    protected synchronized TaskExecutionListener getTaskExecutionLister() {
      return parentRunnableTask == null ? null : parentRunnableTask.getTaskExecutionLister();
    }

    public synchronized void setSubTaskGroupType(SubTaskGroupType subTaskGroupType) {
      if (taskInfo.getSubTaskGroupType() != subTaskGroupType) {
        taskInfo.setSubTaskGroupType(subTaskGroupType);
        taskInfo.save();
      }
    }

    private synchronized void setRunnableTaskContext(
        RunnableTask parentRunnableTask, int position) {
      this.parentRunnableTask = parentRunnableTask;
      taskInfo.setParentUuid(parentRunnableTask.getTaskUUID());
      taskInfo.setPosition(position);
      taskInfo.save();
    }
  }
}
