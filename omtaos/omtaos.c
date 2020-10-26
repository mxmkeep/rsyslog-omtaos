/* omtaos.c
 * copy and modify ommysql file
 * insert data into tdengine
 */
#include "config.h"
#include "rsyslog.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <netdb.h>
#include <taos.h>
#include "conf.h"
#include "syslogd-types.h"
#include "srUtils.h"
#include "template.h"
#include "module-template.h"
#include "errmsg.h"
#include "cfsysline.h"
#include "../omelasticsearch/cJSON/cjson.h"

MODULE_TYPE_OUTPUT
MODULE_TYPE_NOKEEP
MODULE_CNFNAME("omtaos")

static rsRetVal resetConfigVariables(uchar __attribute__((unused)) *pp, void __attribute__((unused)) *pVal);

/* internal structures
 */
DEF_OMOD_STATIC_DATA
DEFobjCurrIf(errmsg)

typedef struct _instanceData {
	char	dbsrv[MAXHOSTNAMELEN+1];	/* IP or hostname of DB server*/ 
	unsigned int dbsrvPort;		/* port of taos server */
	char	dbname[_DB_MAXDBLEN+1];	/* DB name */
	char	dbuid[_DB_MAXUNAMELEN+1];	/* DB user */
	char	dbpwd[_DB_MAXPWDLEN+1];	/* DB user's password */
	uchar   *configfile;			/* taos Client Configuration File */
	uchar   *configsection;		/* taos Client Configuration Section */
	uchar	*tplName;			/* format template to use */
} instanceData;

typedef struct wrkrInstanceData {
	instanceData *pData;
	TAOS	*htaos;			/* handle to taos */
	unsigned uLasttaosErrno;		/* last errno returned by taos or 0 if all is well */
	//pthread_mutex_t taosmutex;
	//uint32_t    count;
} wrkrInstanceData_t;

typedef struct configSettings_s {
	int iSrvPort;				/* database server port */
	uchar *psztaosConfigFile;	/* taos Client Configuration File */
	uchar *psztaosConfigSection;	/* taos Client Configuration Section */
} configSettings_t;
static configSettings_t cs;

typedef struct carray_s {
    char       *start;
    uint32_t    len;
} carray_t;
#define CARRAY_MAX_LEN  1000
#define SQL_MAX_LEN     6000

/* tables for interfacing with the v6 config system */
/* action (instance) parameters */
static struct cnfparamdescr actpdescr[] = {
	{ "server", eCmdHdlrGetWord, 1 },
	{ "db", eCmdHdlrGetWord, 1 },
	{ "uid", eCmdHdlrGetWord, 1 },
	{ "pwd", eCmdHdlrGetWord, 1 },
	{ "serverport", eCmdHdlrInt, 0 },
	{ "taosconfig.file", eCmdHdlrGetWord, 0 },
	{ "taosconfig.section", eCmdHdlrGetWord, 0 },
	{ "template", eCmdHdlrGetWord, 0 }
};
static struct cnfparamblk actpblk =
	{ CNFPARAMBLK_VERSION,
	  sizeof(actpdescr)/sizeof(struct cnfparamdescr),
	  actpdescr
	};


BEGINinitConfVars		/* (re)set config variables to default values */
CODESTARTinitConfVars 
	resetConfigVariables(NULL, NULL);
ENDinitConfVars


BEGINcreateInstance
CODESTARTcreateInstance
ENDcreateInstance


BEGINcreateWrkrInstance
CODESTARTcreateWrkrInstance
	pWrkrData->htaos = NULL;
ENDcreateWrkrInstance


BEGINisCompatibleWithFeature
CODESTARTisCompatibleWithFeature
	if(eFeat == sFEATURERepeatedMsgReduction)
		iRet = RS_RET_OK;
ENDisCompatibleWithFeature


/* The following function is responsible for closing a
 * taos connection.
 * Initially added 2004-10-28
 */
static void closetaos(wrkrInstanceData_t *pWrkrData)
{
    //errmsg.LogError(0, NO_ERRCODE, "taos:%s ", __FUNCTION__);
    dbgprintf("%s: close \n", __FUNCTION__);
	if(pWrkrData->htaos != NULL) {	/* just to be on the safe side... */
	    errmsg.LogError(0, NO_ERRCODE, "%s:close %s",__FUNCTION__, pWrkrData->pData->dbname);
		taos_close(pWrkrData->htaos);
		pWrkrData->htaos = NULL;
	}
}

BEGINfreeInstance
CODESTARTfreeInstance
	free(pData->configfile);
	free(pData->configsection);
	free(pData->tplName);
ENDfreeInstance


BEGINfreeWrkrInstance
CODESTARTfreeWrkrInstance
	closetaos(pWrkrData);
    //pthread_mutex_destroy(&pWrkrData->taosmutex);
ENDfreeWrkrInstance


BEGINdbgPrintInstInfo
CODESTARTdbgPrintInstInfo
	/* nothing special here */
ENDdbgPrintInstInfo


/* log a database error with descriptive message.
 * We check if we have a valid taos handle. If not, we simply
 * report an error, but can not be specific. RGerhards, 2007-01-30
 */
static void reportDBError(wrkrInstanceData_t *pWrkrData, int bSilent)
{
	char errMsg[512];
	unsigned utaosErrno;
	/* output log message */
	errno = 0;
	//errmsg.LogError(0, NO_ERRCODE, "taos:%s in", __FUNCTION__);
	if(pWrkrData->htaos == NULL) {
		errmsg.LogError(0, NO_ERRCODE, "unknown DB error occured - could not obtain taos handle");
	} else { /* we can ask taos for the error description... */
		utaosErrno = taos_errno(pWrkrData->htaos);
		snprintf(errMsg, sizeof(errMsg), "db error (%d): %s\n", utaosErrno,
		        taos_errstr(pWrkrData->htaos));
		if(bSilent || utaosErrno == pWrkrData->uLasttaosErrno)
			dbgprintf("taos, DBError(silent): %s\n", errMsg);
		else {
			pWrkrData->uLasttaosErrno = utaosErrno;
			errmsg.LogError(0, NO_ERRCODE, "%s", errMsg);
		}
	}
		
	return;
}


/* The following function is responsible for initializing a
 * taos connection.
 * Initially added 2004-10-28 mmeckelein
 */
static rsRetVal inittaos(wrkrInstanceData_t *pWrkrData, int bSilent)
{
	instanceData *pData;
	DEFiRet;
	if(pWrkrData->htaos)
	    RETiRet;
	//ASSERT(pWrkrData->htaos == NULL);
	pData = pWrkrData->pData;
	taos_init();
	pWrkrData->htaos = taos_connect(pData->dbsrv, pData->dbuid,pData->dbpwd, pData->dbname, pData->dbsrvPort);
	dbgprintf("%s: ip=%s uid=%s pwd=%s dbname=%s port=%u\n",
	        __FUNCTION__ ,
	        pData->dbsrv, pData->dbuid,pData->dbpwd, pData->dbname, pData->dbsrvPort
	        );
	errmsg.LogError(0, NO_ERRCODE, "%s:connect to %s",__FUNCTION__, pWrkrData->pData->dbname);
    if(pWrkrData->htaos == NULL) {
        errmsg.LogError(0, NO_ERRCODE, "%s connect error %s", __FUNCTION__, taos_errstr(pWrkrData->htaos));
        reportDBError(pWrkrData, bSilent);
        closetaos(pWrkrData); /* ignore any error we may get */
        ABORT_FINALIZE(RS_RET_SUSPENDED);
    }


finalize_it:
	RETiRet;
}


static char* taos_time_local_to_ts(wrkrInstanceData_t *pWrkrData, carray_t* tms, char* val)
{
    //"time_local":"03/Sep/2020:17:14:52 +0800"
//    static char  *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
//                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    //tm  2020-09-15 16:21:13.260728

    //dbgprintf( "%s: tm=%s \n", __FUNCTION__, tm->start);

    char* tm = tms->start;
    memcpy(val, tm+7, 4);       //2020
    val += 4;
    *val++ = '-';

    switch(tm[3])
    {
    case 'J':
        if(tm[4] == 'a'){
            *val++ = '0';
            *val++ = '1';
        } else {
            if(tm[5] == 'n'){
                *val++ = '0';
                *val++ = '6';
            } else {
                *val++ = '0';
                *val++ = '7';
            }
        }
        break;
    case 'F':
        *val++ = '0';
        *val++ = '2';
        break;
    case 'M':
        if(tm[5] == 'r'){
            *val++ = '0';
            *val++ = '3';
        } else {
            *val++ = '0';
            *val++ = '5';
        }
        break;
    case 'A':
        if(tm[4] == 'p'){
            *val++ = '0';
            *val++ = '4';
        } else {
            *val++ = '0';
            *val++ = '8';
        }
        break;
    case 'S':
        *val++ = '0';
        *val++ = '9';
        break;
    case 'O':
        *val++ = '1';
        *val++ = '0';
        break;
    case 'N':
        *val++ = '1';
        *val++ = '1';
        break;
    case 'D':
        *val++ = '1';
        *val++ = '2';
        break;
    }

    *val++ = '-';
    *val++ = tm[0];
    *val++ = tm[1];
    *val++ = ' ';
    memcpy(val, tm+12, 8);
    val += 8;
    *val++ = '.';

    uint32_t ct = 0;
//    pthread_mutex_lock(&pWrkrData->taosmutex);
//    pWrkrData->count++;
//    if(pWrkrData->count > 999999)
//        pWrkrData->count = 0;
//    ct = pWrkrData->count;
//    pthread_mutex_unlock(&pWrkrData->taosmutex);

    static uint32_t counter = 0;
    __sync_add_and_fetch(&counter, 1);
    ct = counter%1000000;

    snprintf(val, 7, "%06d", ct);
    val += 6;

    dbgprintf( "%s: new time_local=%s \n", __FUNCTION__, (val-26));
//    errmsg.LogError(0, NO_ERRCODE, "%s: new time_local=%s",
//            __FUNCTION__, (val-26));

    return val;
}

/* The following function writes the current log entry
 * to an established taos session.
 * Initially added 2004-10-28 mmeckelein
 */
static rsRetVal writetaos(wrkrInstanceData_t *pWrkrData, char *psz)
{
	DEFiRet;
#if 0
	cJSON *root = cJSON_Parse((const char *)psz);
    if (!root) {
        errmsg.LogError(0, NO_ERRCODE, "%s: wrong json msg=%s",
                __FUNCTION__, psz);
        RETiRet;

    }

    cJSON *msg = cJSON_GetObjectItem(root, "msg");
    cJSON *sql = cJSON_GetObjectItem(root, "sql");
    if ( !msg || !sql) {
        errmsg.LogError(0, NO_ERRCODE, "%s2: wrong json msg=%s",
                __FUNCTION__, psz);
        cJSON_Delete(root);
        RETiRet;
    }
#endif

    if(strlen(psz) < 10){
        errmsg.LogError(0, NO_ERRCODE, "%s: msg=%s strlen < 10",
                __FUNCTION__, psz);
        RETiRet;
    }

    char* msg_pos = psz;
    char* sql_pos = NULL;
    carray_t val_arr[CARRAY_MAX_LEN];
    memset(val_arr, 0, sizeof(carray_t)*CARRAY_MAX_LEN);
    int msg_num = 1;
    while(*msg_pos) {
        if(*msg_pos=='*' && *(msg_pos+1)==':' && *(msg_pos+2)=='*' && *(msg_pos+3)==':') {
            sql_pos = msg_pos+4;
            break;
        }
        if(*msg_pos == '"') {
            if(val_arr[msg_num].start == NULL)
                val_arr[msg_num].start = msg_pos+1;
            else {
                val_arr[msg_num].len = msg_pos - val_arr[msg_num].start;
                ++msg_num;
                if(msg_num > CARRAY_MAX_LEN){
                    errmsg.LogError(0, NO_ERRCODE, "%s: msg key > %d",
                                   __FUNCTION__, CARRAY_MAX_LEN);
                    break;
                }
            }
        }
        ++msg_pos;
    }

    dbgprintf( "%s:msg_num=%d\n", __FUNCTION__, msg_num);

//    int i = 1;
//    for(; i < msg_num; i++ )
//    {
//        char ddd[2000] = {0};
//        memcpy(ddd, val_arr[i].start, val_arr[i].len);
//        dbgprintf( "%s:msg_num=%d msg=%s\n", __FUNCTION__, i, ddd);
//    }


    char sql_mem[SQL_MAX_LEN] = {0};
    memset(sql_mem, 0, SQL_MAX_LEN);
    char* sql_val = sql_mem;
    char* keystart = NULL;
    //char* keyend = NULL;
    int bStar = 0;
    while(sql_pos && *sql_pos) {

        if(*sql_pos == '*')
            bStar = 1;
        else
            bStar = 0;

        if(!bStar) {
            *sql_val = *sql_pos;
            sql_val++;
            sql_pos++;
            continue;
        }

        keystart = ++sql_pos;
        while(*sql_pos && *sql_pos != '*')
            sql_pos++;
        char key[50] = {0};
        int key_len = sql_pos - keystart;
        memcpy(key, keystart, key_len>50?50:key_len);
        //char* key_val = NULL;
        //int vallen = 0;
        if(strncmp(key, "time_local_to_ts", strlen("time_local_to_ts")) == 0) {
            //cJSON *tm_lc = cJSON_GetObjectItem(msg, "time_local");

            int timepos = atoi(key+strlen("time_local_to_ts")+1);
            if(timepos >= msg_num || val_arr[timepos].len != 26) {
                errmsg.LogError(0, NO_ERRCODE, "%s:wrong time_local key=%s msg_num=%d msg=%s",
                               __FUNCTION__, key, msg_num, psz);
                RETiRet;
            }
            sql_val = taos_time_local_to_ts(pWrkrData, &(val_arr[timepos]), sql_val);

        } else {
            //cJSON *jk_val = cJSON_GetObjectItem(msg, key);
            int keypos = atoi(key);
            //dbgprintf( "%s:key=%d len=%d\n", __FUNCTION__, keypos, val_arr[keypos].len );
            if(keypos < msg_num && val_arr[keypos].len > 0) {
                if(sql_val + val_arr[keypos].len >= sql_mem+SQL_MAX_LEN)
                {
                    errmsg.LogError(0, NO_ERRCODE, "%s: sql len> %d, sql_mem=%s",
                                   __FUNCTION__, SQL_MAX_LEN, sql_mem);
                    RETiRet;
                }
                char oldchar = *(val_arr[keypos].start+val_arr[keypos].len);
                *(val_arr[keypos].start+val_arr[keypos].len) = '\0';
                sql_val += snprintf(sql_val, val_arr[keypos].len+1, "%s", val_arr[keypos].start);
                *(val_arr[keypos].start+val_arr[keypos].len) = oldchar;
            }
        }
        //dbgprintf( "%s:sqllen=%d vallen=%d sql=%s\n", __FUNCTION__, sql_val-sql_mem,vallen, sql_mem);
        keystart = NULL;
        sql_pos++;
    }
    //cJSON_Delete(root);

	char* pos = sql_mem;
	while(*(pos+1)) {
	    if(*pos == ';')     //replace ';' to '$'
	        *pos = '$';
	    if(*pos == '-' && *(pos+1) == ',')  //replace '0-' to '00'
	        *pos = '0';
	    ++pos;
	}
	dbgprintf( "%s:sql %s\n", __FUNCTION__, sql_mem);
	/* see if we are ready to proceed */
	if(pWrkrData->htaos == NULL) {
		CHKiRet(inittaos(pWrkrData, 0));
		RETiRet;
	}
	TAOS_RES *tret = NULL;

	if(pWrkrData->htaos) {
        /* try insert */
	    tret = taos_query(pWrkrData->htaos, (char*)sql_mem);
        if(tret == NULL || taos_errno(tret) != 0) {
            errmsg.LogError(0, NO_ERRCODE, "%s:insert error err=%s no=%d sql=%s",
                    __FUNCTION__, taos_errstr(pWrkrData->htaos), taos_errno(tret), sql_mem);
            /* error occured, try to re-init connection and retry */
//            closetaos(pWrkrData); /* close the current handle */
//            CHKiRet(inittaos(pWrkrData, 0)); /* try to re-open */
//            if(taos_query(pWrkrData->htaos, (char*)sql_mem)) { /* re-try insert */
//                /* we failed, giving up for now */
//                reportDBError(pWrkrData, 0);
//                closetaos(pWrkrData); /* free ressources */
//                ABORT_FINALIZE(RS_RET_SUSPENDED);
//            }
        }else {
            dbgprintf("%s: err=%s no=%d\n", __FUNCTION__,
                    taos_errstr(pWrkrData->htaos), taos_errno(tret));
        }
	}

finalize_it:
	if(iRet == RS_RET_OK) {
		pWrkrData->uLasttaosErrno = 0; /* reset error for error supression */
	}

	if(tret)taos_free_result(tret);
	RETiRet;
}


BEGINtryResume
CODESTARTtryResume
	if(pWrkrData->htaos == NULL) {
		iRet = inittaos(pWrkrData, 1);
	}
ENDtryResume

BEGINbeginTransaction
CODESTARTbeginTransaction
	CHKiRet(inittaos(pWrkrData, 0));
finalize_it:
ENDbeginTransaction

BEGINdoAction
CODESTARTdoAction
	CHKiRet(writetaos(pWrkrData, ppString[0]));
	iRet = RS_RET_DEFER_COMMIT;
finalize_it:
ENDdoAction

BEGINendTransaction
CODESTARTendTransaction
    dbgprintf( "%s endTransaction\n", __FUNCTION__);
//	if(taos_commit(pWrkrData->htaos) != 0)	{
//		dbgprintf("taos server error: transaction not committed\n");
//		iRet = RS_RET_SUSPENDED;
//	}
ENDendTransaction


static inline void
setInstParamDefaults(instanceData *pData)
{
	pData->dbsrvPort = 0;
	pData->configfile = NULL;
	pData->configsection = NULL;
	pData->tplName = NULL;
}


/* note: we use the fixed-size buffers inside the config object to avoid
 * changing too much of the previous plumbing. rgerhards, 2012-02-02
 */
BEGINnewActInst
	struct cnfparamvals *pvals;
	int i;
	char *cstr;
CODESTARTnewActInst
	if((pvals = nvlstGetParams(lst, &actpblk, NULL)) == NULL) {
		ABORT_FINALIZE(RS_RET_MISSING_CNFPARAMS);
	}

	CHKiRet(createInstance(&pData));
	setInstParamDefaults(pData);

	CODE_STD_STRING_REQUESTparseSelectorAct(1)
	for(i = 0 ; i < actpblk.nParams ; ++i) {
		if(!pvals[i].bUsed)
			continue;
		if(!strcmp(actpblk.descr[i].name, "server")) {
			cstr = es_str2cstr(pvals[i].val.d.estr, NULL);
			strncpy(pData->dbsrv, cstr, sizeof(pData->dbsrv));
			free(cstr);
		} else if(!strcmp(actpblk.descr[i].name, "serverport")) {
			pData->dbsrvPort = (int) pvals[i].val.d.n;
		} else if(!strcmp(actpblk.descr[i].name, "db")) {
			cstr = es_str2cstr(pvals[i].val.d.estr, NULL);
			strncpy(pData->dbname, cstr, sizeof(pData->dbname));
			free(cstr);
		} else if(!strcmp(actpblk.descr[i].name, "uid")) {
			cstr = es_str2cstr(pvals[i].val.d.estr, NULL);
			strncpy(pData->dbuid, cstr, sizeof(pData->dbuid));
			free(cstr);
		} else if(!strcmp(actpblk.descr[i].name, "pwd")) {
			cstr = es_str2cstr(pvals[i].val.d.estr, NULL);
			strncpy(pData->dbpwd, cstr, sizeof(pData->dbpwd));
			free(cstr);
		} else if(!strcmp(actpblk.descr[i].name, "taosconfig.file")) {
			pData->configfile = (uchar*)es_str2cstr(pvals[i].val.d.estr, NULL);
		} else if(!strcmp(actpblk.descr[i].name, "taosconfig.section")) {
			pData->configsection = (uchar*)es_str2cstr(pvals[i].val.d.estr, NULL);
		} else if(!strcmp(actpblk.descr[i].name, "template")) {
			pData->tplName = (uchar*)es_str2cstr(pvals[i].val.d.estr, NULL);
		} else {
			dbgprintf("omtaos: program error, non-handled "
			  "param '%s'\n", actpblk.descr[i].name);
		}
	}

	if(pData->tplName == NULL) {
		CHKiRet(OMSRsetEntry(*ppOMSR, 0, (uchar*) strdup(" StdDBFmt"),
			OMSR_RQD_TPL_OPT_SQL));
	} else {
		CHKiRet(OMSRsetEntry(*ppOMSR, 0,
			(uchar*) strdup((char*) pData->tplName),
			OMSR_RQD_TPL_OPT_SQL));
	}
CODE_STD_FINALIZERnewActInst
	cnfparamvalsDestruct(pvals, &actpblk);
ENDnewActInst


BEGINparseSelectorAct
	int itaosPropErr = 0;
CODESTARTparseSelectorAct
CODE_STD_STRING_REQUESTparseSelectorAct(1)
	/* first check if this config line is actually for us
	 * The first test [*p == '>'] can be skipped if a module shall only
	 * support the newer slection syntax [:modname:]. This is in fact
	 * recommended for new modules. Please note that over time this part
	 * will be handled by rsyslogd itself, but for the time being it is
	 * a good compromise to do it at the module level.
	 * rgerhards, 2007-10-15
	 */
	if(*p == '>') {
		p++; /* eat '>' '*/
	} else if(!strncmp((char*) p, ":omtaos:", sizeof(":omtaos:") - 1)) {
		p += sizeof(":omtaos:") - 1; /* eat indicator sequence  (-1 because of '\0'!) */
	} else {
		ABORT_FINALIZE(RS_RET_CONFLINE_UNPROCESSED);
	}

	/* ok, if we reach this point, we have something for us */
	CHKiRet(createInstance(&pData));

	/* rger 2004-10-28: added support for taos
	 * >server,dbname,userid,password
	 * Now we read the taos connection properties
	 * and verify that the properties are valid.
	 */
	if(getSubString(&p, pData->dbsrv, MAXHOSTNAMELEN+1, ','))
		itaosPropErr++;
	if(*pData->dbsrv == '\0')
		itaosPropErr++;
	if(getSubString(&p, pData->dbname, _DB_MAXDBLEN+1, ','))
		itaosPropErr++;
	if(*pData->dbname == '\0')
		itaosPropErr++;
	if(getSubString(&p, pData->dbuid, _DB_MAXUNAMELEN+1, ','))
		itaosPropErr++;
	if(*pData->dbuid == '\0')
		itaosPropErr++;
	if(getSubString(&p, pData->dbpwd, _DB_MAXPWDLEN+1, ';'))
		itaosPropErr++;
	/* now check for template
	 * We specify that the SQL option must be present in the template.
	 * This is for your own protection (prevent sql injection).
	 */
	if(*(p-1) == ';')
		--p;	/* TODO: the whole parsing of the taos module needs to be re-thought - but this here
			 *       is clean enough for the time being -- rgerhards, 2007-07-30
			 */
	CHKiRet(cflineParseTemplateName(&p, *ppOMSR, 0, OMSR_RQD_TPL_OPT_SQL, (uchar*) " StdDBFmt"));
	
	/* If we detect invalid properties, we disable logging, 
	 * because right properties are vital at this place.  
	 * Retries make no sense. 
	 */
	if (itaosPropErr) {
		errmsg.LogError(0, RS_RET_INVALID_PARAMS, "Trouble with taos connection properties. -taos logging disabled");
		ABORT_FINALIZE(RS_RET_INVALID_PARAMS);
	} else {
		pData->dbsrvPort = (unsigned) cs.iSrvPort;	/* set configured port */
		pData->configfile = cs.psztaosConfigFile;
		pData->configsection = cs.psztaosConfigSection;
	}

CODE_STD_FINALIZERparseSelectorAct
ENDparseSelectorAct


BEGINmodExit
CODESTARTmodExit
//#	ifdef HAVE_TAOS_LIBRARY_INIT
//	taos_library_end();
//#	else
//	taos_server_end();
//#	endif
ENDmodExit


BEGINqueryEtryPt
CODESTARTqueryEtryPt
CODEqueryEtryPt_STD_OMOD_QUERIES
CODEqueryEtryPt_STD_OMOD8_QUERIES
CODEqueryEtryPt_STD_CONF2_OMOD_QUERIES
CODEqueryEtryPt_TXIF_OMOD_QUERIES /* we support the transactional interface! */
ENDqueryEtryPt


/* Reset config variables for this module to default values.
 */
static rsRetVal resetConfigVariables(uchar __attribute__((unused)) *pp, void __attribute__((unused)) *pVal)
{
	DEFiRet;
	cs.iSrvPort = 0; /* zero is the default port */
	free(cs.psztaosConfigFile);
	cs.psztaosConfigFile = NULL;
	free(cs.psztaosConfigSection);
	cs.psztaosConfigSection = NULL;
	RETiRet;
}

BEGINmodInit()
CODESTARTmodInit
INITLegCnfVars
	*ipIFVersProvided = CURR_MOD_IF_VERSION; /* we only support the current interface specification */
CODEmodInit_QueryRegCFSLineHdlr
	CHKiRet(objUse(errmsg, CORE_COMPONENT));
	INITChkCoreFeature(bCoreSupportsBatching, CORE_FEATURE_BATCHING);
	if(!bCoreSupportsBatching) {	
		errmsg.LogError(0, NO_ERRCODE, "omtaos: rsyslog core too old");
		ABORT_FINALIZE(RS_RET_ERR);
	}

	/* we need to init the taos library. If that fails, we cannot run */
//	if(
//#	ifdef HAVE_TAOS_LIBRARY_INIT
//	   taos_library_init(0, NULL, NULL)
//#	else
//	   taos_server_init(0, NULL, NULL)
//#	endif
//	                                   ) {
//		errmsg.LogError(0, NO_ERRCODE, "omtaos: intializing taos client failed, plugin "
//		                "can not run");
//		ABORT_FINALIZE(RS_RET_ERR);
//	}

	/* register our config handlers */
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"actionomtaosserverport", 0, eCmdHdlrInt, NULL, &cs.iSrvPort, STD_LOADABLE_MODULE_ID));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"omtaosconfigfile",0,eCmdHdlrGetWord,NULL,&cs.psztaosConfigFile,STD_LOADABLE_MODULE_ID));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"omtaosconfigsection",0,eCmdHdlrGetWord,NULL,&cs.psztaosConfigSection,STD_LOADABLE_MODULE_ID));
	CHKiRet(omsdRegCFSLineHdlr((uchar *)"resetconfigvariables", 1, eCmdHdlrCustomHandler, resetConfigVariables, NULL, STD_LOADABLE_MODULE_ID));
ENDmodInit

/* vi:set ai:
 */
