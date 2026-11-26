/* rsyslog output module that writes Table-model SQL to Apache IoTDB REST. */
#include "config.h"
#include "rsyslog.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "conf.h"
#include "syslogd-types.h"
#include "srUtils.h"
#include "template.h"
#include "module-template.h"
#include "errmsg.h"
#include "cfsysline.h"
#include "omiotdb_rest.h"

#define OMIOTDB_SQL_CAPACITY OMIOTDB_MAX_REQUEST
#define OMIOTDB_SCHEMA_CAPACITY OMIOTDB_MAX_REQUEST
#define OMIOTDB_ERROR_CAPACITY 512

MODULE_TYPE_OUTPUT
MODULE_TYPE_NOKEEP
MODULE_CNFNAME("omiotdb")

DEF_OMOD_STATIC_DATA
DEFobjCurrIf(errmsg)

typedef struct _instanceData {
	char *server;
	char *database;
	char *table;
	char *columns;
	char *username;
	char *password;
	char *templateName;
	long timeoutMs;
} instanceData;

typedef struct _wrkrInstanceData {
	instanceData *pData;
	CURL *curl;
	struct curl_slist *headers;
	int schemaReady;
} wrkrInstanceData_t;

typedef struct _responseBuffer {
	char *data;
	size_t capacity;
	size_t length;
} responseBuffer;

static struct cnfparamdescr actpdescr[] = {
	{ "server", eCmdHdlrGetWord, 1 },
	{ "db", eCmdHdlrGetWord, 1 },
	{ "table", eCmdHdlrGetWord, 1 },
	{ "columns", eCmdHdlrString, 1 },
	{ "uid", eCmdHdlrGetWord, 1 },
	{ "pwd", eCmdHdlrString, 1 },
	{ "timeout", eCmdHdlrInt, 0 },
	{ "template", eCmdHdlrGetWord, 1 }
};
static struct cnfparamblk actpblk = {
	CNFPARAMBLK_VERSION,
	sizeof(actpdescr) / sizeof(struct cnfparamdescr),
	actpdescr
};

static size_t receive_response(char *data, size_t size, size_t count, void *opaque)
{
	responseBuffer *response = (responseBuffer *) opaque;
	size_t bytes;
	if(size != 0 && count > (size_t)-1 / size)
		return 0;
	bytes = size * count;
	if(bytes >= response->capacity - response->length)
		return 0;
	memcpy(response->data + response->length, data, bytes);
	response->length += bytes;
	response->data[response->length] = '\0';
	return bytes;
}

static int post_iotdb(void *context, const char *url,
		const char *username, const char *password, long timeout_ms,
		const char *body, long *http_status, char *response,
		size_t response_capacity, char *error, size_t error_capacity)
{
	wrkrInstanceData_t *pWrkrData = (wrkrInstanceData_t *) context;
	responseBuffer responseData;
	char curlError[CURL_ERROR_SIZE];
	CURLcode result;
	long responseCode = 0;

	if(pWrkrData == NULL || pWrkrData->curl == NULL || pWrkrData->headers == NULL ||
		url == NULL || username == NULL || password == NULL || body == NULL ||
		http_status == NULL || response == NULL || response_capacity == 0) {
		if(error != NULL && error_capacity > 0)
			snprintf(error, error_capacity, "IoTDB HTTP client is not initialized");
		return 0;
	}
	response[0] = '\0';
	curlError[0] = '\0';
	responseData.data = response;
	responseData.capacity = response_capacity;
	responseData.length = 0;
	curl_easy_reset(pWrkrData->curl);

#define SET_CURL_OPTION(option, value) \
	do { \
		result = curl_easy_setopt(pWrkrData->curl, option, value); \
		if(result != CURLE_OK) \
			goto curl_option_error; \
	} while(0)

	SET_CURL_OPTION(CURLOPT_URL, url);
	SET_CURL_OPTION(CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
	SET_CURL_OPTION(CURLOPT_USERNAME, username);
	SET_CURL_OPTION(CURLOPT_PASSWORD, password);
	SET_CURL_OPTION(CURLOPT_POST, 1L);
	SET_CURL_OPTION(CURLOPT_POSTFIELDS, body);
	SET_CURL_OPTION(CURLOPT_POSTFIELDSIZE, (long) strlen(body));
	SET_CURL_OPTION(CURLOPT_HTTPHEADER, pWrkrData->headers);
	SET_CURL_OPTION(CURLOPT_WRITEFUNCTION, receive_response);
	SET_CURL_OPTION(CURLOPT_WRITEDATA, &responseData);
	SET_CURL_OPTION(CURLOPT_ERRORBUFFER, curlError);
	SET_CURL_OPTION(CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
	SET_CURL_OPTION(CURLOPT_TIMEOUT_MS, timeout_ms);
	SET_CURL_OPTION(CURLOPT_NOSIGNAL, 1L);
	SET_CURL_OPTION(CURLOPT_FOLLOWLOCATION, 0L);

	result = curl_easy_perform(pWrkrData->curl);
	if(result != CURLE_OK) {
		if(error != NULL && error_capacity > 0)
			snprintf(error, error_capacity, "IoTDB REST transport failed: %.300s",
				curlError[0] == '\0' ? curl_easy_strerror(result) : curlError);
		return 0;
	}
	result = curl_easy_getinfo(pWrkrData->curl, CURLINFO_RESPONSE_CODE, &responseCode);
	if(result != CURLE_OK) {
		if(error != NULL && error_capacity > 0)
			snprintf(error, error_capacity, "could not read IoTDB HTTP status: %s",
				curl_easy_strerror(result));
		return 0;
	}
	*http_status = responseCode;
	return 1;

curl_option_error:
	if(error != NULL && error_capacity > 0)
		snprintf(error, error_capacity, "could not configure IoTDB HTTP client: %s",
			curl_easy_strerror(result));
	return 0;
#undef SET_CURL_OPTION
}

BEGINcreateInstance
CODESTARTcreateInstance
ENDcreateInstance

BEGINcreateWrkrInstance
CODESTARTcreateWrkrInstance
	pWrkrData->curl = curl_easy_init();
	pWrkrData->headers = NULL;
	pWrkrData->schemaReady = 0;
	if(pWrkrData->curl == NULL) {
		errmsg.LogError(0, NO_ERRCODE, "omiotdb: could not create libcurl handle");
		ABORT_FINALIZE(RS_RET_ERR);
	}
	pWrkrData->headers = curl_slist_append(NULL, "Content-Type: application/json");
	if(pWrkrData->headers == NULL) {
		errmsg.LogError(0, NO_ERRCODE, "omiotdb: could not allocate HTTP headers");
		ABORT_FINALIZE(RS_RET_OUT_OF_MEMORY);
	}
finalize_it:
	if(iRet != RS_RET_OK) {
		if(pWrkrData->curl != NULL)
			curl_easy_cleanup(pWrkrData->curl);
		free(pWrkrData);
		pWrkrData = NULL;
	}
ENDcreateWrkrInstance

BEGINisCompatibleWithFeature
CODESTARTisCompatibleWithFeature
	if(eFeat == sFEATURERepeatedMsgReduction)
		iRet = RS_RET_OK;
ENDisCompatibleWithFeature

BEGINfreeInstance
CODESTARTfreeInstance
	free(pData->server);
	free(pData->database);
	free(pData->table);
	free(pData->columns);
	free(pData->username);
	free(pData->password);
	free(pData->templateName);
ENDfreeInstance

BEGINfreeWrkrInstance
CODESTARTfreeWrkrInstance
	if(pWrkrData->headers != NULL) {
		curl_slist_free_all(pWrkrData->headers);
		pWrkrData->headers = NULL;
	}
	if(pWrkrData->curl != NULL) {
		curl_easy_cleanup(pWrkrData->curl);
		pWrkrData->curl = NULL;
	}
ENDfreeWrkrInstance

BEGINdbgPrintInstInfo
CODESTARTdbgPrintInstInfo
	dbgprintf("omiotdb: server=%s db=%s table=%s timeout=%ldms\n",
		pData->server, pData->database, pData->table, pData->timeoutMs);
ENDdbgPrintInstInfo

BEGINtryResume
CODESTARTtryResume
	if(pWrkrData->curl == NULL || pWrkrData->headers == NULL)
		iRet = RS_RET_SUSPENDED;
ENDtryResume

BEGINbeginTransaction
CODESTARTbeginTransaction
	if(!pWrkrData->schemaReady) {
		char createDatabase[512];
		char createTable[OMIOTDB_SCHEMA_CAPACITY];
		char error[OMIOTDB_ERROR_CAPACITY] = {0};
		instanceData *pData = pWrkrData->pData;
		int written = snprintf(createDatabase, sizeof(createDatabase),
			"CREATE DATABASE IF NOT EXISTS %s", pData->database);
		if(written < 0 || (size_t) written >= sizeof(createDatabase) ||
			!iotdb_build_create_table_sql(pData->table, pData->columns,
				createTable, sizeof(createTable), error, sizeof(error))) {
			errmsg.LogError(0, NO_ERRCODE, "omiotdb: invalid schema configuration: %s",
				error[0] == '\0' ? "database name is too long" : error);
			ABORT_FINALIZE(RS_RET_ERR);
		}
		if(!iotdb_execute(pData->server, "", pData->username, pData->password,
			pData->timeoutMs, createDatabase, post_iotdb, pWrkrData,
			error, sizeof(error))) {
			errmsg.LogError(0, NO_ERRCODE, "omiotdb: database initialization failed: %s", error);
			ABORT_FINALIZE(RS_RET_SUSPENDED);
		}
		if(!iotdb_execute(pData->server, pData->database, pData->username,
			pData->password, pData->timeoutMs, createTable, post_iotdb,
			pWrkrData, error, sizeof(error))) {
			errmsg.LogError(0, NO_ERRCODE, "omiotdb: table initialization failed: %s", error);
			ABORT_FINALIZE(RS_RET_SUSPENDED);
		}
		pWrkrData->schemaReady = 1;
	}
finalize_it:
ENDbeginTransaction

BEGINdoAction
CODESTARTdoAction
	{
		char sql[OMIOTDB_SQL_CAPACITY];
		char error[OMIOTDB_ERROR_CAPACITY] = {0};
		if(!iotdb_expand_template((const char *) ppString[0], sql, sizeof(sql),
			error, sizeof(error))) {
			errmsg.LogError(0, NO_ERRCODE, "omiotdb: message template expansion failed: %s", error);
			ABORT_FINALIZE(RS_RET_ERR);
		}
		if(!iotdb_execute(pWrkrData->pData->server, pWrkrData->pData->database,
			pWrkrData->pData->username, pWrkrData->pData->password,
			pWrkrData->pData->timeoutMs, sql, post_iotdb, pWrkrData,
			error, sizeof(error))) {
			errmsg.LogError(0, NO_ERRCODE, "omiotdb: write failed: %s", error);
			ABORT_FINALIZE(RS_RET_SUSPENDED);
		}
	}
	iRet = RS_RET_DEFER_COMMIT;
finalize_it:
ENDdoAction

BEGINendTransaction
CODESTARTendTransaction
ENDendTransaction

BEGINnewActInst
	struct cnfparamvals *pvals;
	int i;
CODESTARTnewActInst
	if((pvals = nvlstGetParams(lst, &actpblk, NULL)) == NULL)
		ABORT_FINALIZE(RS_RET_MISSING_CNFPARAMS);
	CHKiRet(createInstance(&pData));
	pData->timeoutMs = 5000;
	CODE_STD_STRING_REQUESTnewActInst(1)
	for(i = 0; i < actpblk.nParams; ++i) {
		char *value;
		if(!pvals[i].bUsed)
			continue;
		if(!strcmp(actpblk.descr[i].name, "timeout")) {
			pData->timeoutMs = pvals[i].val.d.n;
			continue;
		}
		value = es_str2cstr(pvals[i].val.d.estr, NULL);
		if(value == NULL)
			ABORT_FINALIZE(RS_RET_OUT_OF_MEMORY);
		if(!strcmp(actpblk.descr[i].name, "server"))
			pData->server = value;
		else if(!strcmp(actpblk.descr[i].name, "db"))
			pData->database = value;
		else if(!strcmp(actpblk.descr[i].name, "table"))
			pData->table = value;
		else if(!strcmp(actpblk.descr[i].name, "columns"))
			pData->columns = value;
		else if(!strcmp(actpblk.descr[i].name, "uid"))
			pData->username = value;
		else if(!strcmp(actpblk.descr[i].name, "pwd"))
			pData->password = value;
		else if(!strcmp(actpblk.descr[i].name, "template"))
			pData->templateName = value;
		else {
			free(value);
			errmsg.LogError(0, NO_ERRCODE, "omiotdb: unhandled action parameter '%s'",
				actpblk.descr[i].name);
			ABORT_FINALIZE(RS_RET_ERR);
		}
	}
	if(pData->server == NULL || pData->database == NULL || pData->table == NULL ||
		pData->columns == NULL || pData->username == NULL || pData->password == NULL ||
		pData->templateName == NULL) {
		errmsg.LogError(0, NO_ERRCODE,
			"omiotdb: server, db, table, columns, uid, pwd, and template are required");
		ABORT_FINALIZE(RS_RET_MISSING_CNFPARAMS);
	}
	{
		char error[OMIOTDB_ERROR_CAPACITY] = {0};
		char schema[OMIOTDB_SCHEMA_CAPACITY];
		if(!iotdb_validate_config(pData->server, pData->database,
			pData->username, pData->password, pData->timeoutMs,
			error, sizeof(error)) ||
			!iotdb_build_create_table_sql(pData->table, pData->columns,
				schema, sizeof(schema), error, sizeof(error))) {
			errmsg.LogError(0, NO_ERRCODE, "omiotdb: invalid action configuration: %s", error);
			ABORT_FINALIZE(RS_RET_INVALID_PARAMS);
		}
	}
	CHKiRet(OMSRsetEntry(*ppOMSR, 0, (uchar *)pData->templateName, 0));
	pData->templateName = NULL; /* ownership transferred to rsyslog */
CODE_STD_FINALIZERnewActInst
	cnfparamvalsDestruct(pvals, &actpblk);
ENDnewActInst

BEGINparseSelectorAct
CODESTARTparseSelectorAct
	CODE_STD_STRING_REQUESTparseSelectorAct(1)
	ABORT_FINALIZE(RS_RET_CONFLINE_UNPROCESSED);
CODE_STD_FINALIZERparseSelectorAct
ENDparseSelectorAct

BEGINmodExit
CODESTARTmodExit
	curl_global_cleanup();
	objRelease(errmsg, CORE_COMPONENT);
ENDmodExit

BEGINqueryEtryPt
CODESTARTqueryEtryPt
CODEqueryEtryPt_STD_OMOD_QUERIES
CODEqueryEtryPt_STD_OMOD8_QUERIES
CODEqueryEtryPt_STD_CONF2_OMOD_QUERIES
CODEqueryEtryPt_TXIF_OMOD_QUERIES
ENDqueryEtryPt

BEGINmodInit()
CODESTARTmodInit
	*ipIFVersProvided = CURR_MOD_IF_VERSION;
CODEmodInit_QueryRegCFSLineHdlr
	CHKiRet(objUse(errmsg, CORE_COMPONENT));
	INITChkCoreFeature(bCoreSupportsBatching, CORE_FEATURE_BATCHING);
	if(!bCoreSupportsBatching) {
		errmsg.LogError(0, NO_ERRCODE, "omiotdb: rsyslog core too old");
		ABORT_FINALIZE(RS_RET_ERR);
	}
	if(curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
		errmsg.LogError(0, NO_ERRCODE, "omiotdb: could not initialize libcurl");
		ABORT_FINALIZE(RS_RET_ERR);
	}
ENDmodInit
