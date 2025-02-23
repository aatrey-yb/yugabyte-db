---
title: Build a Rust application that uses YSQL
headerTitle: Build a Rust application
description: Build a small Rust application using the Rust-Postgres driver and using the YSQL API to connect to and interact with a Yugabyte Cloud cluster.
menu:
  latest:
    parent: cloud-build-apps
    name: Rust
    identifier: cloud-rust
    weight: 800
type: page
isTocNested: true
showAsideToc: true
---

The following tutorial shows a small [Rust application](https://github.com/yugabyte/yugabyte-simple-rust-app) that connects to a YugabyteDB cluster using the [Rust-Postgres driver](../../../../reference/drivers/ysql-client-drivers/#rust-postgres) and performs basic SQL operations. Use the application as a template to get started with Yugabyte Cloud in Rust.

## Prerequisites

- [Rust](https://www.rust-lang.org/tools/install) development environment. The sample application was created for Rust 1.58 but should work for earlier and later versions.

### Yugabyte Cloud

- You have a cluster deployed in Yugabyte Cloud. To get started, use the [Quick start](../../).
- You downloaded the cluster CA certificate. Refer to [Download your cluster certificate](../../../cloud-secure-clusters/cloud-authentication/#download-your-cluster-certificate).
- You have added your computer to the cluster IP allow list. Refer to [Assign IP Allow Lists](../../../cloud-secure-clusters/add-connections/).

## Clone the application from GitHub

Clone the sample application to your computer:

```sh
git clone https://github.com/yugabyte/yugabyte-simple-rust-app && cd yugabyte-simple-rust-app
```

## Provide connection parameters

The application needs to establish a connection to the YugabyteDB cluster. To do this:

1. Open the `sample-app.rs` file in the `src` directory.

2. Set the following configuration-related constants:

    - **HOST** - the host name of your YugabyteDB cluster. To obtain a Yugabyte Cloud cluster host name, sign in to Yugabyte Cloud, select your cluster on the **Clusters** page, and click **Settings**. The host is displayed under **Network Access**.
    - **PORT** - the port number that will be used by the driver (the default YugabyteDB YSQL port is 5433).
    - **DB_NAME** - the name of the database you are connecting to (the default database is named `yugabyte`).
    - **USER** and **PASSWORD** - the username and password for the YugabyteDB database. If you are using the credentials you created when deploying a cluster in Yugabyte Cloud, these can be found in the credentials file you downloaded.
    - **SSL_MODE** - the SSL mode to use. Yugabyte Cloud [requires SSL connections](../../../cloud-secure-clusters/cloud-authentication/#ssl-modes-in-ysql); use `SslMode::Require`.
    - **SSL_ROOT_CERT** - the full path to the Yugabyte Cloud cluster CA certificate.

3. Save the file.

## Build and run the application

Build and run the application.

```sh
$ cargo run
```

The driver is included in the dependencies list of the `Cargo.toml` file and installed automatically the first time you run the application.

You should see output similar to the following:

```output
>>>> Successfully connected to YugabyteDB!
>>>> Successfully created table DemoAccount.
>>>> Selecting accounts:
name = Jessica, age = 28, country = USA, balance = 10000
name = John, age = 28, country = Canada, balance = 9000
>>>> Transferred 800 between accounts.
>>>> Selecting accounts:
name = Jessica, age = 28, country = USA, balance = 9200
name = John, age = 28, country = Canada, balance = 9800
```

You have successfully executed a basic Rust application that works with Yugabyte Cloud.

## Explore the application logic

Open the `sample-app.rs` file in the `yugabyte-simple-rust-app/src` folder to review the methods.

### connect

The `connect` method establishes a connection with your cluster via the Rust-Postgres driver.

```rust
let mut cfg = Config::new();

cfg.host(HOST).port(PORT).dbname(DB_NAME).
    user(USER).password(PASSWORD).ssl_mode(SSL_MODE);

let mut builder = SslConnector::builder(SslMethod::tls())?;
builder.set_ca_file(SSL_ROOT_CERT)?;
let connector = MakeTlsConnector::new(builder.build());

let client = cfg.connect(connector)?;
```

### create_database

The `create_database` method uses PostgreSQL-compliant DDL commands to create a sample database.

```rust
client.execute("DROP TABLE IF EXISTS DemoAccount", &[])?;

client.execute("CREATE TABLE DemoAccount (
                id int PRIMARY KEY,
                name varchar,
                age int,
                country varchar,
                balance int)", &[])?;

client.execute("INSERT INTO DemoAccount VALUES
                (1, 'Jessica', 28, 'USA', 10000),
                (2, 'John', 28, 'Canada', 9000)", &[])?;
```

### select_accounts

The `select_accounts` method queries your distributed data using the SQL `SELECT` statement.

```rust
for row in client.query("SELECT name, age, country, balance FROM DemoAccount", &[])? {
    let name: &str = row.get("name");
    let age: i32 = row.get("age");
    let country: &str = row.get("country");
    let balance: i32 = row.get("balance");

    println!("name = {}, age = {}, country = {}, balance = {}",
        name, age, country, balance);
}
```

### transfer_money_between_accounts

The `transfer_money_between_accounts` method updates your data consistently with distributed transactions.

```rust
let mut txn = client.transaction()?;

let exec_txn = || -> Result<(), DBError> {
    txn.execute("UPDATE DemoAccount SET balance = balance - $1 WHERE name = \'Jessica\'", &[&amount])?;
    txn.execute("UPDATE DemoAccount SET balance = balance + $1 WHERE name = \'John\'", &[&amount])?;
    txn.commit()?;

    Ok(())
};
```

## Learn more

[Rust-Postgres driver](../../../../reference/drivers/ysql-client-drivers/#rust-postgres)

[Explore more applications](../../../cloud-examples/)

[Deploy clusters in Yugabyte Cloud](../../../cloud-basics)

[Connect to applications in Yugabyte Cloud](../../../cloud-connect/connect-applications/)
