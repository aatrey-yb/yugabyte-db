/*--------------------------------------------------------------------------------------------------
 *
 * yb_type.c
 *        Commands for creating and altering table structures and settings
 *
 * Copyright (c) YugaByte, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied.  See the License for the specific language governing permissions and limitations
 * under the License.
 *
 * IDENTIFICATION
 *        src/backend/catalog/yb_catalog/yb_type.c
 *
 * TODO(all) At the mininum we must support the following datatype efficiently as they are used
 * for system tables.
 *   bool
 *   char
 *   text
 *   int2
 *   int4
 *   int8
 *   float4
 *   float8
 *   timestamptz
 *   bytea
 *   oid
 *   xid
 *   cid
 *   tid
 *   name (same as text?)
 *   aclitem
 *   pg_node_tree
 *   pg_lsn
 *   pg_ndistinct
 *   pg_dependencies
 *
 *   OID aliases:
 *
 *   regproc
 *   regprocedure
 *   regoper
 *   regoperator
 *   regclass
 *   regtype
 *   regconfig
 *   regdictionary
 *
 *   Vectors/arrays:
 *
 *   int2vector (list of 16-bit integers)
 *   oidvector (list of 32-bit unsigned integers)
 *   anyarray (list of 32-bit integers - signed or unsigned)
 *--------------------------------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/pg_enum.h"
#include "catalog/pg_type.h"
#include "catalog/yb_type.h"
#include "mb/pg_wchar.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/cash.h"
#include "utils/date.h"
#include "utils/geo_decls.h"
#include "utils/inet.h"
#include "utils/numeric.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

#include "yb/yql/pggate/ybc_pggate.h"

#include "pg_yb_utils.h"

static const YBCPgTypeEntity YBCFixedLenByValTypeEntity;
static const YBCPgTypeEntity YBCNullTermByRefTypeEntity;
static const YBCPgTypeEntity YBCVarLenByRefTypeEntity;

static Datum YbDocdbToDatum(const uint8 *data, int64 bytes, const YBCPgTypeAttrs *type_attrs);
static void YbDatumToDocdb(Datum datum, uint8 **data, int64 *bytes);

/***************************************************************************************************
 * Find YugaByte storage type for each PostgreSQL datatype.
 * NOTE: Because YugaByte network buffer can be deleted after it is processed, Postgres layer must
 *       allocate a buffer to keep the data in its slot.
 **************************************************************************************************/
const YBCPgTypeEntity *
YbDataTypeFromOidMod(int attnum, Oid type_id)
{
	/* Find type for system column */
	if (attnum < InvalidAttrNumber) {
		switch (attnum) {
			case SelfItemPointerAttributeNumber: /* ctid */
				type_id = TIDOID;
				break;
			case ObjectIdAttributeNumber: /* oid */
			case TableOidAttributeNumber: /* tableoid */
				type_id = OIDOID;
				break;
			case MinCommandIdAttributeNumber: /* cmin */
			case MaxCommandIdAttributeNumber: /* cmax */
				type_id = CIDOID;
				break;
			case MinTransactionIdAttributeNumber: /* xmin */
			case MaxTransactionIdAttributeNumber: /* xmax */
				type_id = XIDOID;
				break;
			case YBTupleIdAttributeNumber:            /* ybctid */
			case YBIdxBaseTupleIdAttributeNumber:     /* ybidxbasectid */
			case YBUniqueIdxKeySuffixAttributeNumber: /* ybuniqueidxkeysuffix */
				type_id = BYTEAOID;
				break;
			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("System column not yet supported in YugaByte: %d", attnum)));
				break;
		}
	}

	/* Find the type mapping entry */
	const YBCPgTypeEntity *type_entity = YBCPgFindTypeEntity(type_id);
	YBCPgDataType yb_type = YBCPgGetType(type_entity);

	/* For non-primitive types, we need to look up the definition */
	if (yb_type == YB_YQL_DATA_TYPE_UNKNOWN_DATA) {
		HeapTuple type = typeidType(type_id);
		Form_pg_type tp = (Form_pg_type) GETSTRUCT(type);
		Oid basetp_oid = tp->typbasetype;
		ReleaseSysCache(type);

		switch (tp->typtype) {
			case TYPTYPE_BASE:
				if (tp->typbyval) {
					/* fixed-length, pass-by-value base type */
					return &YBCFixedLenByValTypeEntity;
				} else {
					switch (tp->typlen) {
						case -2:
							/* null-terminated, pass-by-reference base type */
							return &YBCNullTermByRefTypeEntity;
							break;
						case -1:
							/* variable-length, pass-by-reference base type */
							return &YBCVarLenByRefTypeEntity;
							break;
						default:;
							/* fixed-length, pass-by-reference base type */
							YBCPgTypeEntity *fixed_ref_type_entity = (YBCPgTypeEntity *)palloc(
									sizeof(YBCPgTypeEntity));
							fixed_ref_type_entity->type_oid = InvalidOid;
							fixed_ref_type_entity->yb_type = YB_YQL_DATA_TYPE_BINARY;
							fixed_ref_type_entity->allow_for_primary_key = false;
							fixed_ref_type_entity->datum_fixed_size = tp->typlen;
							fixed_ref_type_entity->datum_to_yb = (YBCPgDatumToData)YbDatumToDocdb;
							fixed_ref_type_entity->yb_to_datum =
								(YBCPgDatumFromData)YbDocdbToDatum;
							return fixed_ref_type_entity;
							break;
					}
				}
				break;
			case TYPTYPE_COMPOSITE:
				basetp_oid = RECORDOID;
				break;
			case TYPTYPE_DOMAIN:
				break;
			case TYPTYPE_ENUM:
				basetp_oid = ANYENUMOID;
				break;
			case TYPTYPE_RANGE:
				basetp_oid = ANYRANGEOID;
				break;
			default:
				YB_REPORT_TYPE_NOT_SUPPORTED(type_id);
				break;
		}
		return YbDataTypeFromOidMod(InvalidAttrNumber, basetp_oid);
	}

	/* Report error if type is not supported */
	if (yb_type == YB_YQL_DATA_TYPE_NOT_SUPPORTED) {
		YB_REPORT_TYPE_NOT_SUPPORTED(type_id);
	}

	/* Return the type-mapping entry */
	return type_entity;
}

bool
YbDataTypeIsValidForKey(Oid type_id)
{
	const YBCPgTypeEntity *type_entity = YbDataTypeFromOidMod(InvalidAttrNumber, type_id);
	return YBCPgAllowForPrimaryKey(type_entity);
}

const YBCPgTypeEntity *
YbDataTypeFromName(TypeName *typeName)
{
	Oid   type_id = 0;
	int32 typmod  = 0;

	typenameTypeIdAndMod(NULL /* parseState */ , typeName, &type_id, &typmod);
	return YbDataTypeFromOidMod(InvalidAttrNumber, type_id);
}

/***************************************************************************************************
 * Conversion Functions.
 **************************************************************************************************/
/*
 * BOOL conversion.
 * Fixed size: Ignore the "bytes" data size.
 */
void YbDatumToBool(Datum datum, bool *data, int64 *bytes) {
	*data = DatumGetBool(datum);
}

Datum YbBoolToDatum(const bool *data, int64 bytes, const YBCPgTypeAttrs *type_attrs) {
	return BoolGetDatum(*data);
}

/*
 * BINARY conversion.
 */
void YbDatumToBinary(Datum datum, void **data, int64 *bytes) {
	*data = VARDATA_ANY(datum);
	*bytes = VARSIZE_ANY_EXHDR(datum);
}

Datum YbBinaryToDatum(const void *data, int64 bytes, const YBCPgTypeAttrs *type_attrs) {
	/* PostgreSQL can represent text strings up to 1 GB minus a four-byte header. */
	if (bytes > kYBCMaxPostgresTextSizeBytes || bytes < 0) {
		ereport(ERROR, (errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
						errmsg("Invalid data size")));
	}
	return PointerGetDatum(cstring_to_text_with_len(data, bytes));
}

/*
 * TEXT type conversion.
 */
void YbDatumToText(Datum datum, char **data, int64 *bytes) {
	*data = VARDATA_ANY(datum);
	*bytes = VARSIZE_ANY_EXHDR(datum);
}

Datum YbTextToDatum(const char *data, int64 bytes, const YBCPgTypeAttrs *type_attrs) {
	/* While reading back TEXT from storage, we don't need to check for data length. */
	return PointerGetDatum(cstring_to_text_with_len(data, bytes));
}

/*
 * CHAR type conversion.
 */
void YbDatumToChar(Datum datum, char *data, int64 *bytes) {
	*data = DatumGetChar(datum);
}

Datum YbCharToDatum(const char *data, int64 bytes, const YBCPgTypeAttrs *type_attrs) {
	return CharGetDatum(*data);
}

/*
 * CHAR-based type conversion.
 */
void YbDatumToBPChar(Datum datum, char **data, int64 *bytes) {
	int size;
	*data = TextDatumGetCString(datum);

	/*
	 * Right trim all spaces on the right. For CHAR(n) - BPCHAR - datatype, Postgres treats space
	 * characters at tail-end the same as '\0' characters.
	 *   "abc  " == "abc"
	 * Left spaces don't have this special behaviors.
	 *   "  abc" != "abc"
	 */
	size = strlen(*data);
	while (size > 0 && isspace((*data)[size - 1])) {
		size--;
	}
	*bytes = size;
}

Datum YbBPCharToDatum(const char *data, int64 bytes, const YBCPgTypeAttrs *type_attrs) {
	/* PostgreSQL can represent text strings up to 1 GB minus a four-byte header. */
	if (bytes > kYBCMaxPostgresTextSizeBytes || bytes < 0) {
		ereport(ERROR, (errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
						errmsg("Invalid data size")));
	}

	/* Convert YugaByte cstring to Postgres internal representation */
	FunctionCallInfoData fargs;
	FunctionCallInfo fcinfo = &fargs;
	PG_GETARG_DATUM(0) = CStringGetDatum(data);
	PG_GETARG_DATUM(2) = Int32GetDatum(type_attrs->typmod);
	return bpcharin(fcinfo);
}

void YbDatumToVarchar(Datum datum, char **data, int64 *bytes) {
	*data = TextDatumGetCString(datum);
	*bytes = strlen(*data);
}

Datum YbVarcharToDatum(const char *data, int64 bytes, const YBCPgTypeAttrs *type_attrs) {
	/* PostgreSQL can represent text strings up to 1 GB minus a four-byte header. */
	if (bytes > kYBCMaxPostgresTextSizeBytes || bytes < 0) {
		ereport(ERROR, (errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
						errmsg("Invalid data size")));
	}

	/* Convert YugaByte cstring to Postgres internal representation */
	FunctionCallInfoData fargs;
	FunctionCallInfo fcinfo = &fargs;
	PG_GETARG_DATUM(0) = CStringGetDatum(data);
	PG_GETARG_DATUM(2) = Int32GetDatum(type_attrs->typmod);
	return varcharin(fcinfo);
}

/*
 * NAME conversion.
 */
void YbDatumToName(Datum datum, char **data, int64 *bytes) {
	*data = DatumGetCString(datum);
	*bytes = strlen(*data);
}

Datum YbNameToDatum(const char *data, int64 bytes, const YBCPgTypeAttrs *type_attrs) {
	/* PostgreSQL can represent text strings up to 1 GB minus a four-byte header. */
	if (bytes > kYBCMaxPostgresTextSizeBytes || bytes < 0) {
		ereport(ERROR, (errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
						errmsg("Invalid data size")));
	}

	/* Truncate oversize input */
	if (bytes >= NAMEDATALEN)
		bytes = pg_mbcliplen(data, bytes, NAMEDATALEN - 1);

	/* We use palloc0 here to ensure result is zero-padded */
	Name result = (Name)palloc0(NAMEDATALEN);
	memcpy(NameStr(*result), data, bytes);
	return NameGetDatum(result);
}

/*
 * PSEUDO-type cstring conversion.
 * Not a type that is used by users.
 */
void YbDatumToCStr(Datum datum, char **data, int64 *bytes) {
	*data = DatumGetCString(datum);
	*bytes = strlen(*data);
}

Datum YbCStrToDatum(const char *data, int64 bytes, const YBCPgTypeAttrs *type_attrs) {
	/* PostgreSQL can represent text strings up to 1 GB minus a four-byte header. */
	if (bytes > kYBCMaxPostgresTextSizeBytes || bytes < 0) {
		ereport(ERROR, (errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
						errmsg("Invalid data size")));
	}

	/* Convert YugaByte cstring to Postgres internal representation */
	FunctionCallInfoData fargs;
	FunctionCallInfo fcinfo = &fargs;
	PG_GETARG_DATUM(0) = CStringGetDatum(data);
	return cstring_in(fcinfo);
}

/*
 * INTEGERs conversion.
 * Fixed size: Ignore the "bytes" data size.
 */
void YbDatumToInt16(Datum datum, int16 *data, int64 *bytes) {
	*data = DatumGetInt16(datum);
}

Datum YbInt16ToDatum(const int16 *data, int64 bytes, const YBCPgTypeAttrs *type_attrs) {
	return Int16GetDatum(*data);
}

void YbDatumToInt32(Datum datum, int32 *data, int64 *bytes) {
	*data = DatumGetInt32(datum);
}

Datum YbInt32ToDatum(const int32 *data, int64 bytes, const YBCPgTypeAttrs *type_attrs) {
	return Int32GetDatum(*data);
}

void YbDatumToInt64(Datum datum, int64 *data, int64 *bytes) {
	*data = DatumGetInt64(datum);
}

Datum YbInt64ToDatum(const int64 *data, int64 bytes, const YBCPgTypeAttrs *type_attrs) {
	return Int64GetDatum(*data);
}

void YbDatumToUInt64(Datum datum, uint64 *data, uint64 *bytes) {
        *data = DatumGetUInt64(datum);
}

Datum YbUInt64ToDatum(const uint64 *data, uint64 bytes, const YBCPgTypeAttrs *type_attrs) {
        return UInt64GetDatum(*data);
}

/*
 * Given datum representing a 4-byte enum oid, lookup its sort order which is
 * a 4-byte float, then treat the sort order as a 4-byte integer. Combine
 * the sort order with the enum oid to make an int64 by putting the sort order
 * at the high 4-byte and the enum oid at the low 4-byte.
 */
void YbDatumToEnum(Datum datum, int64 *data, int64 *bytes) {
	if (!bytes) {
		HeapTuple tup;
		Form_pg_enum en;
		uint32_t sort_order;

		/*
		 * We expect datum to only contain a enum oid and does not already contain a sort order.
		 * For OID >= 2147483648, Postgres sign-extends datum with 0xffffffff, which is -NaN and
		 * does not reprensent a valid sort order.
		 */
		Assert(!(datum >> 32) || ((datum >> 32) == 0xffffffff));

		/* Clear the high 4-byte in case it is not zero. */
		datum &= 0xffffffff;

		/*
		 * Find the sort order of this enum oid.
		 */
		tup = SearchSysCache1(ENUMOID, datum);
		Assert(tup);
		en = (Form_pg_enum) GETSTRUCT(tup);
		sort_order = *(uint32 *) (&en->enumsortorder);

		/*
		 * Place the sort order at the high 4-byte of datum.
		 */
		datum |= ((int64) sort_order) << 32;
		ReleaseSysCache(tup);
	} else {
		/*
		 * If the caller passes a non-null address to bytes then it means it requests
		 * us to not add sort order to datum for testing purpose.
		 */
	}
	*data = DatumGetInt64(datum);
}

Datum YbEnumToDatum(const int64 *data, int64 bytes, const YBCPgTypeAttrs *type_attrs) {
	// Clear the sort order from the higher 4-bytes.
	return Int64GetDatum(*data) & 0xffffffffLL;
}

void YbDatumToOid(Datum datum, Oid *data, int64 *bytes) {
	*data = DatumGetObjectId(datum);
}

Datum YbOidToDatum(const Oid *data, int64 bytes, const YBCPgTypeAttrs *type_attrs) {
	return ObjectIdGetDatum(*data);
}

void YbDatumToCommandId(Datum datum, CommandId *data, int64 *bytes) {
	*data = DatumGetCommandId(datum);
}

Datum YbCommandIdToDatum(const CommandId *data, int64 bytes, const YBCPgTypeAttrs *type_attrs) {
	return CommandIdGetDatum(*data);
}

void YbDatumToTransactionId(Datum datum, TransactionId *data, int64 *bytes) {
	*data = DatumGetTransactionId(datum);
}

Datum YbTransactionIdToDatum(const TransactionId *data, int64 bytes,
							  const YBCPgTypeAttrs *type_attrs) {
	return TransactionIdGetDatum(*data);
}

/*
 * FLOATs conversion.
 * Fixed size: Ignore the "bytes" data size.
 */
void YbDatumToFloat4(Datum datum, float *data, int64 *bytes) {
	*data = DatumGetFloat4(datum);
}

Datum YbFloat4ToDatum(const float *data, int64 bytes, const YBCPgTypeAttrs *type_attrs) {
	return Float4GetDatum(*data);
}

void YbDatumToFloat8(Datum datum, double *data, int64 *bytes) {
	*data = DatumGetFloat8(datum);
}

Datum YbFloat8ToDatum(const double *data, int64 bytes, const YBCPgTypeAttrs *type_attrs) {
	return Float8GetDatum(*data);
}

/*
 * DECIMAL / NUMERIC conversion.
 * We're using plaintext c-string as an intermediate step between PG and YB numerics.
 */
void YbDatumToDecimalText(Datum datum, char *plaintext[], int64 *bytes) {
	Numeric num = DatumGetNumeric(datum);
	*plaintext = numeric_normalize(num);
	// NaN support will be added in ENG-4645
	if (strncmp(*plaintext, "NaN", 3) == 0) {
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("DECIMAL does not support NaN yet")));
	}
}

Datum YbDecimalTextToDatum(const char plaintext[], int64 bytes, const YBCPgTypeAttrs *type_attrs) {
	FunctionCallInfoData fargs;
	FunctionCallInfo fcinfo = &fargs;
	PG_GETARG_DATUM(0) = CStringGetDatum(plaintext);
	PG_GETARG_DATUM(2) = Int32GetDatum(type_attrs->typmod);
	return numeric_in(fcinfo);
}

/*
 * MONEY conversion.
 * We're using int64 as a representation, just like Postgres does.
 */
void YbDatumToMoneyInt64(Datum datum, int64 *data, int64 *bytes) {
	*data = DatumGetCash(datum);
}

Datum YbMoneyInt64ToDatum(const int64 *data, int64 bytes, const YBCPgTypeAttrs *type_attrs) {
	return CashGetDatum(*data);
}

/*
 * UUID Datatype.
 */
void YbDatumToUuid(Datum datum, unsigned char **data, int64 *bytes) {
	// Postgres store uuid as hex string.
	*data = (DatumGetUUIDP(datum))->data;
	*bytes = UUID_LEN;
}

Datum YbUuidToDatum(const unsigned char *data, int64 bytes, const YBCPgTypeAttrs *type_attrs) {
	// We have to make a copy for data because the "data" pointer belongs to YugaByte cache memory
	// which can be cleared at any time.
	pg_uuid_t *uuid;
	if (bytes != UUID_LEN) {
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
						errmsg("Unexpected size for UUID (%ld)", bytes)));
	}

	uuid = (pg_uuid_t *)palloc(sizeof(pg_uuid_t));
	memcpy(uuid->data, data, UUID_LEN);
	return UUIDPGetDatum(uuid);
}

/*
 * DATE conversions.
 * PG represents DATE as signed int32 number of days since 2000-01-01, we store it as-is
 */

void YbDatumToDate(Datum datum, int32 *data, int64 *bytes) {
	*data = DatumGetDateADT(datum);
}

Datum YbDateToDatum(const int32 *data, int64 bytes, const YBCPgTypeAttrs* type_attrs) {
	return DateADTGetDatum(*data);
}

/*
 * TIME conversions.
 * PG represents TIME as microseconds in int64, we store it as-is
 */

void YbDatumToTime(Datum datum, int64 *data, int64 *bytes) {
	*data = DatumGetTimeADT(datum);
}

Datum YbTimeToDatum(const int64 *data, int64 bytes, const YBCPgTypeAttrs *type_attrs) {
	return TimeADTGetDatum(*data);
}

/*
 * INTERVAL conversions.
 * PG represents INTERVAL as 128 bit structure, store it as binary
 */
void YbDatumToInterval(Datum datum, void **data, int64 *bytes) {
	*data = DatumGetIntervalP(datum);
	*bytes = sizeof(Interval);
}

Datum YbIntervalToDatum(const void *data, int64 bytes, const YBCPgTypeAttrs *type_attrs) {
	const size_t sz = sizeof(Interval);
	if (bytes != sz) {
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
						errmsg("Unexpected size for Interval (%ld)", bytes)));
	}
	Interval* result = palloc(sz);
	memcpy(result, data, sz);
	return IntervalPGetDatum(result);
}

void YbDatumToGinNull(Datum datum, uint8 *data, int64 *bytes)
{
	*data = DatumGetUInt8(datum);
}

Datum YbGinNullToDatum(const uint8 *data,
					   int64 bytes,
					   const YBCPgTypeAttrs *type_attrs)
{
	return UInt8GetDatum(*data);
}

/*
 * Workaround: These conversion functions can be used as a quick workaround to support a type.
 * - Used for Datum that contains address or pointer of actual data structure.
 *     Datum = pointer to { 1 or 4 bytes for data-size | data }
 * - Save Datum exactly as-is in YugaByte storage when writing.
 * - Read YugaByte storage and copy as-is to Postgres's in-memory datum when reading.
 *
 * IMPORTANT NOTE: This doesn't work for data values that are cached in-place instead of in a
 * separate space to which the datum is pointing to. For example, it doesn't work for numeric
 * values such as int64_t.
 *   int64_value = (int64)(datum)
 *   Datum = (cached_in_place_datatype)(data)
 */

void YbDatumToDocdb(Datum datum, uint8 **data, int64 *bytes) {
	if (*bytes < 0) {
		*bytes = VARSIZE_ANY(datum);
	}
	*data = (uint8 *)datum;
}

Datum YbDocdbToDatum(const uint8 *data, int64 bytes, const YBCPgTypeAttrs *type_attrs) {
	uint8 *result = palloc(bytes);
	memcpy(result, data, bytes);
	return PointerGetDatum(result);
}

/*
 * Other conversions.
 */

/***************************************************************************************************
 * Conversion Table
 * Contain function pointers for conversion between PostgreSQL Datum to YugaByte data.
 *
 * TODO(Alex)
 * - Change NOT_SUPPORTED to proper datatype.
 * - Turn ON or OFF certain type for KEY (true or false) when testing its support.
 **************************************************************************************************/
static const YBCPgTypeEntity YbTypeEntityTable[] = {
	{ BOOLOID, YB_YQL_DATA_TYPE_BOOL, true, sizeof(bool),
		(YBCPgDatumToData)YbDatumToBool,
		(YBCPgDatumFromData)YbBoolToDatum },

	{ BYTEAOID, YB_YQL_DATA_TYPE_BINARY, true, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ CHAROID, YB_YQL_DATA_TYPE_INT8, true, -1,
		(YBCPgDatumToData)YbDatumToChar,
		(YBCPgDatumFromData)YbCharToDatum },

	{ NAMEOID, YB_YQL_DATA_TYPE_STRING, true, -1,
		(YBCPgDatumToData)YbDatumToName,
		(YBCPgDatumFromData)YbNameToDatum },

	{ INT8OID, YB_YQL_DATA_TYPE_INT64, true, sizeof(int64),
		(YBCPgDatumToData)YbDatumToInt64,
		(YBCPgDatumFromData)YbInt64ToDatum },

	{ INT2OID, YB_YQL_DATA_TYPE_INT16, true, sizeof(int16),
		(YBCPgDatumToData)YbDatumToInt16,
		(YBCPgDatumFromData)YbInt16ToDatum },

	{ INT2VECTOROID, YB_YQL_DATA_TYPE_BINARY, true, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ INT4OID, YB_YQL_DATA_TYPE_INT32, true, sizeof(int32),
		(YBCPgDatumToData)YbDatumToInt32,
		(YBCPgDatumFromData)YbInt32ToDatum },

	{ REGPROCOID, YB_YQL_DATA_TYPE_UINT32, true, sizeof(Oid),
		(YBCPgDatumToData)YbDatumToOid,
		(YBCPgDatumFromData)YbOidToDatum },

	{ TEXTOID, YB_YQL_DATA_TYPE_STRING, true, -1,
		(YBCPgDatumToData)YbDatumToText,
		(YBCPgDatumFromData)YbTextToDatum },

	{ OIDOID, YB_YQL_DATA_TYPE_UINT32, true, sizeof(Oid),
		(YBCPgDatumToData)YbDatumToOid,
		(YBCPgDatumFromData)YbOidToDatum },

	{ TIDOID, YB_YQL_DATA_TYPE_BINARY, false, sizeof(ItemPointerData),
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ XIDOID, YB_YQL_DATA_TYPE_UINT32, true, sizeof(TransactionId),
		(YBCPgDatumToData)YbDatumToTransactionId,
		(YBCPgDatumFromData)YbTransactionIdToDatum },

	{ CIDOID, YB_YQL_DATA_TYPE_UINT32, false, sizeof(CommandId),
		(YBCPgDatumToData)YbDatumToCommandId,
		(YBCPgDatumFromData)YbCommandIdToDatum },

	{ OIDVECTOROID, YB_YQL_DATA_TYPE_BINARY, true, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ JSONOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ JSONARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ XMLOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ XMLARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ PGNODETREEOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ PGNDISTINCTOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ PGDEPENDENCIESOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ PGDDLCOMMANDOID, YB_YQL_DATA_TYPE_INT64, true, sizeof(int64),
		(YBCPgDatumToData)YbDatumToInt64,
		(YBCPgDatumFromData)YbInt64ToDatum },

	{ SMGROID, YB_YQL_DATA_TYPE_INT16, true, sizeof(int16),
		(YBCPgDatumToData)YbDatumToInt16,
		(YBCPgDatumFromData)YbInt16ToDatum },

	{ POINTOID, YB_YQL_DATA_TYPE_BINARY, false, sizeof(Point),
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ LSEGOID, YB_YQL_DATA_TYPE_BINARY, false, sizeof(LSEG),
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ PATHOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ BOXOID, YB_YQL_DATA_TYPE_BINARY, false, sizeof(BOX),
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ POLYGONOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ LINEOID, YB_YQL_DATA_TYPE_BINARY, false, sizeof(LINE),
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ LINEARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ FLOAT4OID, YB_YQL_DATA_TYPE_FLOAT, true, sizeof(int64),
		(YBCPgDatumToData)YbDatumToFloat4,
		(YBCPgDatumFromData)YbFloat4ToDatum },

	{ FLOAT8OID, YB_YQL_DATA_TYPE_DOUBLE, true, sizeof(int64),
		(YBCPgDatumToData)YbDatumToFloat8,
		(YBCPgDatumFromData)YbFloat8ToDatum },

	/* Deprecated datatype in postgres since 6.3 release */
	{ ABSTIMEOID, YB_YQL_DATA_TYPE_NOT_SUPPORTED, true, sizeof(int32),
		(YBCPgDatumToData)YbDatumToInt32,
		(YBCPgDatumFromData)YbInt32ToDatum },

	/* Deprecated datatype in postgres since 6.3 release */
	{ RELTIMEOID, YB_YQL_DATA_TYPE_NOT_SUPPORTED, false, -1,
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	/* Deprecated datatype in postgres since 6.3 release */
	{ TINTERVALOID, YB_YQL_DATA_TYPE_NOT_SUPPORTED, false, -1,
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	/* Deprecated datatype in postgres since 6.3 release */
	{ UNKNOWNOID, YB_YQL_DATA_TYPE_NOT_SUPPORTED, false, -1,
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ CIRCLEOID, YB_YQL_DATA_TYPE_BINARY, false, sizeof(CIRCLE),
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ CIRCLEARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	/* We're using int64 to represent monetary type, just like Postgres does. */
	{ CASHOID, YB_YQL_DATA_TYPE_INT64, true, sizeof(int64),
		(YBCPgDatumToData)YbDatumToMoneyInt64,
		(YBCPgDatumFromData)YbMoneyInt64ToDatum },

	{ MONEYARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ MACADDROID, YB_YQL_DATA_TYPE_BINARY, false, sizeof(macaddr),
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ INETOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ CIDROID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ CIDRARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ MACADDR8OID, YB_YQL_DATA_TYPE_BINARY, false, sizeof(macaddr8),
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ BOOLARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ BYTEAARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ CHARARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ NAMEARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ INT2ARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ INT2VECTORARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ INT4ARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ REGPROCARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ TEXTARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ OIDARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ TIDARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ XIDARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ CIDARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ OIDVECTORARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ BPCHARARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ VARCHARARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ INT8ARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ POINTARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ LSEGARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ PATHARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ BOXARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ FLOAT4ARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ FLOAT8ARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ ABSTIMEARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ RELTIMEARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ TINTERVALARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ POLYGONARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ ACLITEMOID, YB_YQL_DATA_TYPE_BINARY, false, sizeof(AclItem),
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ ACLITEMARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ MACADDRARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ MACADDR8ARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ INETARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ CIDARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ CSTRINGARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ BPCHAROID, YB_YQL_DATA_TYPE_STRING, true, -1,
		(YBCPgDatumToData)YbDatumToBPChar,
		(YBCPgDatumFromData)YbBPCharToDatum },

	{ VARCHAROID, YB_YQL_DATA_TYPE_STRING, true, -1,
		(YBCPgDatumToData)YbDatumToVarchar,
		(YBCPgDatumFromData)YbVarcharToDatum },

	{ DATEOID, YB_YQL_DATA_TYPE_INT32, true, sizeof(int32),
		(YBCPgDatumToData)YbDatumToDate,
		(YBCPgDatumFromData)YbDateToDatum },

	{ TIMEOID, YB_YQL_DATA_TYPE_INT64, true, sizeof(int64),
		(YBCPgDatumToData)YbDatumToTime,
		(YBCPgDatumFromData)YbTimeToDatum },

	{ TIMESTAMPOID, YB_YQL_DATA_TYPE_INT64, true, sizeof(int64),
		(YBCPgDatumToData)YbDatumToInt64,
		(YBCPgDatumFromData)YbInt64ToDatum },

	{ TIMESTAMPARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ DATEARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ TIMEARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ TIMESTAMPTZOID, YB_YQL_DATA_TYPE_INT64, true, sizeof(int64),
		(YBCPgDatumToData)YbDatumToInt64,
		(YBCPgDatumFromData)YbInt64ToDatum },

	{ TIMESTAMPTZARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ INTERVALOID, YB_YQL_DATA_TYPE_BINARY, false, sizeof(Interval),
		(YBCPgDatumToData)YbDatumToInterval,
		(YBCPgDatumFromData)YbIntervalToDatum },

	{ INTERVALARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ NUMERICARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ TIMETZOID, YB_YQL_DATA_TYPE_BINARY, false, sizeof(TimeTzADT),
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ TIMETZARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ BITOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ BITARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ VARBITOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ VARBITARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ NUMERICOID, YB_YQL_DATA_TYPE_DECIMAL, true, -1,
		(YBCPgDatumToData)YbDatumToDecimalText,
		(YBCPgDatumFromData)YbDecimalTextToDatum },

	{ REFCURSOROID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ REGPROCEDUREOID, YB_YQL_DATA_TYPE_UINT32, true, sizeof(Oid),
		(YBCPgDatumToData)YbDatumToOid,
		(YBCPgDatumFromData)YbOidToDatum },

	{ REGOPEROID, YB_YQL_DATA_TYPE_UINT32, true, sizeof(Oid),
		(YBCPgDatumToData)YbDatumToOid,
		(YBCPgDatumFromData)YbOidToDatum },

	{ REGOPERATOROID, YB_YQL_DATA_TYPE_UINT32, true, sizeof(Oid),
		(YBCPgDatumToData)YbDatumToOid,
		(YBCPgDatumFromData)YbOidToDatum },

	{ REGCLASSOID, YB_YQL_DATA_TYPE_UINT32, true, sizeof(Oid),
		(YBCPgDatumToData)YbDatumToOid,
		(YBCPgDatumFromData)YbOidToDatum },

	{ REGTYPEOID, YB_YQL_DATA_TYPE_UINT32, true, sizeof(Oid),
		(YBCPgDatumToData)YbDatumToOid,
		(YBCPgDatumFromData)YbOidToDatum },

	{ REGROLEOID, YB_YQL_DATA_TYPE_UINT32, true, sizeof(Oid),
		(YBCPgDatumToData)YbDatumToOid,
		(YBCPgDatumFromData)YbOidToDatum },

	{ REGNAMESPACEOID, YB_YQL_DATA_TYPE_UINT32, true, sizeof(Oid),
		(YBCPgDatumToData)YbDatumToOid,
		(YBCPgDatumFromData)YbOidToDatum },

	{ REGPROCEDUREARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ REGOPERARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ REGOPERATORARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ REGCLASSARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ REGTYPEARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ REGROLEARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ REGNAMESPACEARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ UUIDOID, YB_YQL_DATA_TYPE_BINARY, true, -1,
		(YBCPgDatumToData)YbDatumToUuid,
		(YBCPgDatumFromData)YbUuidToDatum },

	{ UUIDARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ LSNOID, YB_YQL_DATA_TYPE_UINT64, true, sizeof(uint64),
		(YBCPgDatumToData)YbDatumToUInt64,
		(YBCPgDatumFromData)YbUInt64ToDatum },

	{ PG_LSNARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ TSVECTOROID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ GTSVECTOROID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ TSQUERYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ REGCONFIGOID, YB_YQL_DATA_TYPE_UINT32, true, sizeof(Oid),
		(YBCPgDatumToData)YbDatumToOid,
		(YBCPgDatumFromData)YbOidToDatum },

	{ REGDICTIONARYOID, YB_YQL_DATA_TYPE_UINT32, true, sizeof(Oid),
		(YBCPgDatumToData)YbDatumToOid,
		(YBCPgDatumFromData)YbOidToDatum },

	{ TSVECTORARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ GTSVECTORARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ TSQUERYARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ REGCONFIGARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ REGDICTIONARYARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ JSONBOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ JSONBARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ TXID_SNAPSHOTOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ TXID_SNAPSHOTARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ INT4RANGEOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ INT4RANGEARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ NUMRANGEOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ NUMRANGEARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ TSRANGEOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ TSRANGEARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ TSTZRANGEOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ TSTZRANGEARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ DATERANGEOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ DATERANGEARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ INT8RANGEOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ INT8RANGEARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ RECORDOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	{ RECORDARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },

	/* Length(cstring) == -2 to be consistent with Postgres's 'typlen' attribute */
	{ CSTRINGOID, YB_YQL_DATA_TYPE_STRING, true, -2,
		(YBCPgDatumToData)YbDatumToCStr,
		(YBCPgDatumFromData)YbCStrToDatum },

	{ ANYARRAYOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum },

	{ VOIDOID, YB_YQL_DATA_TYPE_INT64, true, sizeof(int64),
		(YBCPgDatumToData)YbDatumToInt64,
		(YBCPgDatumFromData)YbInt64ToDatum },

	{ TRIGGEROID, YB_YQL_DATA_TYPE_UINT32, true, sizeof(Oid),
		(YBCPgDatumToData)YbDatumToOid,
		(YBCPgDatumFromData)YbOidToDatum },

	{ EVTTRIGGEROID, YB_YQL_DATA_TYPE_UINT32, true, sizeof(Oid),
		(YBCPgDatumToData)YbDatumToOid,
		(YBCPgDatumFromData)YbOidToDatum },

	{ LANGUAGE_HANDLEROID, YB_YQL_DATA_TYPE_UINT32, true, sizeof(Oid),
		(YBCPgDatumToData)YbDatumToOid,
		(YBCPgDatumFromData)YbOidToDatum },

	{ INTERNALOID, YB_YQL_DATA_TYPE_INT64, true, sizeof(int64),
		(YBCPgDatumToData)YbDatumToInt64,
		(YBCPgDatumFromData)YbInt64ToDatum },

	{ OPAQUEOID, YB_YQL_DATA_TYPE_INT32, true, sizeof(int32),
		(YBCPgDatumToData)YbDatumToInt32,
		(YBCPgDatumFromData)YbInt32ToDatum },

	{ ANYELEMENTOID, YB_YQL_DATA_TYPE_INT32, true, sizeof(int32),
		(YBCPgDatumToData)YbDatumToInt32,
		(YBCPgDatumFromData)YbInt32ToDatum },

	{ ANYNONARRAYOID, YB_YQL_DATA_TYPE_INT32, true, sizeof(int32),
		(YBCPgDatumToData)YbDatumToInt32,
		(YBCPgDatumFromData)YbInt32ToDatum },

	{ ANYENUMOID, YB_YQL_DATA_TYPE_INT64, true, sizeof(int64),
		(YBCPgDatumToData)YbDatumToEnum,
		(YBCPgDatumFromData)YbEnumToDatum },

	{ FDW_HANDLEROID, YB_YQL_DATA_TYPE_UINT32, true, sizeof(Oid),
		(YBCPgDatumToData)YbDatumToOid,
		(YBCPgDatumFromData)YbOidToDatum },

	{ INDEX_AM_HANDLEROID, YB_YQL_DATA_TYPE_UINT32, true, sizeof(Oid),
		(YBCPgDatumToData)YbDatumToOid,
		(YBCPgDatumFromData)YbOidToDatum },

	{ TSM_HANDLEROID, YB_YQL_DATA_TYPE_UINT32, true, sizeof(Oid),
		(YBCPgDatumToData)YbDatumToOid,
		(YBCPgDatumFromData)YbOidToDatum },

	{ ANYRANGEOID, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToDocdb,
		(YBCPgDatumFromData)YbDocdbToDatum },
};

/*
 * Special type entity used for fixed-length, pass-by-value user-defined types.
 * TODO(jason): When user-defined types as primary keys are supported, change
 * the below `false` to `true`.
 */
static const YBCPgTypeEntity YBCFixedLenByValTypeEntity =
	{ InvalidOid, YB_YQL_DATA_TYPE_INT64, false, sizeof(int64),
		(YBCPgDatumToData)YbDatumToInt64,
		(YBCPgDatumFromData)YbInt64ToDatum };
/*
 * Special type entity used for null-terminated, pass-by-reference user-defined
 * types.
 * TODO(jason): When user-defined types as primary keys are supported, change
 * the below `false` to `true`.
 */
static const YBCPgTypeEntity YBCNullTermByRefTypeEntity =
	{ InvalidOid, YB_YQL_DATA_TYPE_BINARY, false, -2,
		(YBCPgDatumToData)YbDatumToCStr,
		(YBCPgDatumFromData)YbCStrToDatum };
/*
 * Special type entity used for variable-length, pass-by-reference user-defined
 * types.
 * TODO(jason): When user-defined types as primary keys are supported, change
 * the below `false` to `true`.
 */
static const YBCPgTypeEntity YBCVarLenByRefTypeEntity =
	{ InvalidOid, YB_YQL_DATA_TYPE_BINARY, false, -1,
		(YBCPgDatumToData)YbDatumToBinary,
		(YBCPgDatumFromData)YbBinaryToDatum };

/*
 * Special type entity used for ybgin null categories.
 */
const YBCPgTypeEntity YBCGinNullTypeEntity =
	{ InvalidOid, YB_YQL_DATA_TYPE_GIN_NULL, true, -1,
		(YBCPgDatumToData)YbDatumToGinNull,
		(YBCPgDatumFromData)YbGinNullToDatum };

void YbGetTypeTable(const YBCPgTypeEntity **type_table, int *count) {
	*type_table = YbTypeEntityTable;
	*count = sizeof(YbTypeEntityTable)/sizeof(YBCPgTypeEntity);
}
