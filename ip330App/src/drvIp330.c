/* drvIp330.c

********************COPYRIGHT NOTIFICATION**********************************
This software was developed under a United States Government license
described on the COPYRIGHT_UniversityOfChicago file included as part
of this distribution.
****************************************************************************
    Original Author: Jim Kowalkowski
    Date: 2/15/95
    Current Authors: Mark Rivers, Joe Sullivan, and Marty Kraimer
    Converted to MPF: 12/9/98
    Converted to ip330 (base class) from ip330Scan: MLR 10/27/99

    16-Mar-2000  Mark Rivers.  
                 Added code to only to fppSave and fppRestore if fpp hardware
                 is present.
    28-Oct-2000  Mark Rivers.  
                 Fixed logic in setTimeRegs() to only correct for
                 the 15 microseconds per channel if scanMode is
                 burstContinuous.
                 Logic does uniformContinuous sensibly now,
                 microSecondsPerScan is the time for a
                 complete loop to measure all channels.
    01-Nov-2000  Marty Kraimer.
                 After calibration set mailBoxOffset=16
                 Ignore first sample during calibration. ADC needs settle time.
                 If secondsBetween Calibrate<0 always return raw value
                 Use lock while using/modifying adj_slope and adj_offset
    01-Jan-2001  Ned Arnold, Marty Kraimer
                 Moved statement loading interrupt vector.  This fixes a bug which
                 crashed the IOC if an external trigger is active when the ioc is
                 booted via a power cycle.
    28-Aug-2001  Mark Rivers per Marty Kraimer
                 Removed calls to intClear and intDisable.
                 These were not necessary and did not work on dumb IP carrier.
    12-Oct-2001  Marty Kraimer. Code to handle soft reboots.
    31-Jul-2002  Carl Lionberger and Eric Snow
                 Moved interupt enable to end of config, properly masked mode 
                 control bits in setScanMode, and adjusted mailBoxOffset for 
                 uniformSingle and burstSingle scan modes in intFunc.
    31-Mar-2003  Mark Rivers
                 Minor change, changed all hardcoded values of 32 to 
                 MAX_IP330_CHANNELS
                 Removed calls to intConfig(), Andrew Johnson says they are 
                 not needed.
    10-Jun-2003  Mark Rivers
                 Converted to R3.14.2, mpf-2.2, ipac-2.5
    11-Jul-2004  Mark Rivers.  Converted from MPF to asyn, and from C++ to C
*/

#include <iv.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <taskLib.h>
#include <intLib.h>

#include <drvIpac.h>
#include <iocsh.h>
#include <epicsExport.h>
#include <epicsThread.h>
#include <epicsString.h>
#include <epicsTimer.h>
#include <epicsMutex.h>
#include <epicsMessageQueue.h>
#include <cantProceed.h>
#include <asynDriver.h>
#include <asynInt32.h>
#include <asynFloat64.h>
#include <asynInt32Callback.h>
#include <asynFloat64Callback.h>
#include <asynInt32ArrayCallback.h>

#include "drvIp330.h" 


#define SCAN_DISABLE    0xC8FF  /* control register AND mask - */
                                /* SCAN and INTERRUPT disable */
#define ACROMAG_ID 0xa3
#define ACRO_IP330 0x11

/* Define default configuration parameters */
#define SCAN_MODE burstContinuous
#define TRIGGER_DIRECTION input
#define MICROSECONDS_PER_SCAN 1000
#define SECONDS_BETWEEN_CALIBRATE 0

/* Message queue size */
#define MAX_MESSAGES 100

#define MAX_IP330_CHANNELS 32

typedef enum {differential, singleEnded} signalType;
typedef enum {disable, uniformContinuous, uniformSingle, burstContinuous,
              burstSingle, convertOnExternalTriggerOnly} scanModeType;
typedef enum {input, output} triggerType;

typedef enum{typeInt32, typeFloat64, typeInt32Array} dataType;

#define nRanges 4
#define nGains 4
#define nTriggers 2

static const char *rangeName[nRanges] = {"-5to5","-10to10","0to5","0to10"};
static const char *triggerName[nTriggers] = {"Input", "Output"};
static const double pgaGain[nGains] = {1.0,2.0,4.0,8.0};

typedef struct ip330ADCregs {
    unsigned short control;
    unsigned char timePrescale;
    unsigned char intVector;
    unsigned short conversionTime;
    unsigned char endChanVal;
    unsigned char startChanVal;
    unsigned short newData[2];
    unsigned short missedData[2];
    unsigned short startConvert;
    unsigned char pad[0x0E];
    unsigned char gain[MAX_IP330_CHANNELS];
    unsigned short mailBox[MAX_IP330_CHANNELS];
} ip330ADCregs;

typedef struct ip330ADCSettings {
    double volt_callo;
    double volt_calhi;
    unsigned char ctl_callo;
    unsigned char ctl_calhi;
    double ideal_span;
    double ideal_zero;
    double adj_slope;
    double adj_offset;
    int gain;
} ip330ADCSettings;

typedef struct calibrationSetting {
    double volt_callo;
    double volt_calhi;
    unsigned char ctl_callo;
    unsigned char ctl_calhi;
    double ideal_span;
    double ideal_zero;
} calibrationSetting;

static calibrationSetting calibrationSettings[nRanges][nGains] = {
    {   {0.0000, 4.9000,  0x38,  0x18, 10.0,  -5.0},
        {0.0000, 2.4500,  0x38,  0x20, 10.0,  -5.0},
        {0.0000, 1.2250,  0x38,  0x28, 10.0,  -5.0},
        {0.0000, 0.6125,  0x38,  0x30, 10.0,  -5.0} },
    {   {0.0000, 4.9000,  0x38,  0x18, 20.0, -10.0},
        {0.0000, 4.9000,  0x38,  0x18, 20.0, -10.0},
        {0.0000, 2.4500,  0x38,  0x20, 20.0, -10.0},
        {0.0000, 1.2250,  0x38,  0x28, 20.0, -10.0} },
    {   {0.6125, 4.9000,  0x30,  0x18,  5.0,   0.0},
        {0.6125, 2.4500,  0x30,  0x20,  5.0,   0.0},
        {0.6125, 1.2250,  0x30,  0x28,  5.0,   0.0},
        {0.0000, 0.6125,  0x38,  0x30,  5.0,   0.0} },
    {   {0.6125, 4.9000,  0x30,  0x18, 10.0,   0.0},
        {0.6125, 4.9000,  0x30,  0x18, 10.0,   0.0},
        {0.6125, 2.4500,  0x30,  0x20, 10.0,   0.0},
        {0.6125, 1.2250,  0x30,  0x28, 10.0,   0.0} }
};

typedef struct {
    ELLNODE *next;
    ELLNODE *previous;
    asynInt32Clbk int32Callback;
    asynFloat64Clbk float64Callback;
    asynInt32ArrayClbk int32ArrayCallback;
    void *pvt;
    dataType dataType;
    int channel;
} ip330Client;

typedef struct drvIp330Pvt {
    char *portName;
    asynUser *pasynUser;
    ushort_t carrier;
    ushort_t slot;
    epicsTimerId timerId;
    epicsMutexId lock;
    signalType type;
    int range;
    volatile ip330ADCregs* regs;
    ip330ADCSettings *chanSettings;
    int chanData[MAX_IP330_CHANNELS];
    int correctedData[MAX_IP330_CHANNELS];
    int firstChan;
    int lastChan;
    scanModeType scanMode;
    triggerType trigger;
    int secondsBetweenCalibrate;
    int rebooting;
    int mailBoxOffset;
    epicsMessageQueueId intMsgQId;
    int messagesSent;
    int messagesFailed;
    double actualScanPeriod;
    ELLLIST clientList;
    asynInterface common;
    asynInterface int32;
    asynInterface float64;
    asynInterface int32Callback;
    asynInterface float64Callback;
    asynInterface int32ArrayCallback;
    asynInterface ip330;
} drvIp330Pvt;

/* These functions are used by the interfaces */
static double setScanPeriod         (void *drvPvt, asynUser *pasynUser,
                                     double seconds);
static double getScanPeriod         (void *drvPvt, asynUser *pasynUser);
static asynStatus readInt32         (void *drvPvt, asynUser *pasynUser,
                                     epicsInt32 *value);
static asynStatus writeInt32        (void *drvPvt, asynUser *pasynUser,
                                     epicsInt32 value);
static asynStatus readFloat64       (void *drvPvt, asynUser *pasynUser,
                                     epicsFloat64 *value);
static asynStatus writeFloat64      (void *drvPvt, asynUser *pasynUser,
                                     epicsFloat64 value);
static asynStatus registerInt32Callback (void *drvPvt, asynUser *pasynUser,
                                     asynInt32Clbk callback, void *pvt);
static asynStatus registerFloat64Callback  (void *drvPvt, asynUser *pasynUser,
                                     asynFloat64Clbk callback, void *pvt);
static asynStatus registerInt32ArrayCallback (void *drvPvt, asynUser *pasynUser,
                                     asynInt32ArrayClbk callback, void *pvt);
static void report                  (void *drvPvt, FILE *fp, int details);
static asynStatus connect           (void *drvPvt, asynUser *pasynUser);
static asynStatus disconnect        (void *drvPvt, asynUser *pasynUser);

static asynStatus setGain           (void *drvPvt, asynUser *pasynUser,
                                     int gain);
static asynStatus setSecondsBetweenCalibrate (void *drvPvt, asynUser *pasynUser,
                                              double seconds);

/* These are private functions, not used in any interfaces */
static void intFunc           (void *drvPvt); /* Interrupt function */
static void intTask           (drvIp330Pvt *pPvt);
static int calibrate          (drvIp330Pvt *pPvt, int channel);
static void waitNewData       (drvIp330Pvt *pPvt);
static void autoCalibrate     (void *drvPvt);
static int config             (drvIp330Pvt *pPvt, scanModeType scanMode, 
                               const char *triggerString, 
                               double secondsPerScan, 
                               int secondsBetweenCalibrate);
static int setScanMode        (drvIp330Pvt *pPvt, scanModeType scanMode);
static int setTrigger         (drvIp330Pvt *pPvt, triggerType trigger);
static int setGainPrivate     (drvIp330Pvt *pPvt, int range, int gain, 
                               int channel);
static asynStatus registerCallback(void *drvPvt, asynUser *pasynUser,
                                   void *callback, void *pvt, 
                                   dataType dataType);
     
static const asynCommon drvIp330Common = {
    report,
    connect,
    disconnect
};

static const asynInt32 drvIp330Int32 = {
    writeInt32,
    readInt32
};

static const asynFloat64 drvIp330Float64 = {
    writeFloat64,
    readFloat64
};

static const asynInt32Callback drvIp330Int32Callback = {
    setScanPeriod,
    getScanPeriod,
    registerInt32Callback
};

static const asynInt32ArrayCallback drvIp330Int32ArrayCallback = {
    setScanPeriod,
    getScanPeriod,
    registerInt32ArrayCallback
};

static const asynFloat64Callback drvIp330Float64Callback = {
    setScanPeriod,
    getScanPeriod,
    registerFloat64Callback
};

static const asynIp330 drvIp330 = {
    setGain,
    setSecondsBetweenCalibrate
};


int initIp330(const char *portName, ushort_t carrier, ushort_t slot,
              const char *typeString, const char *rangeString,
              int firstChan, int lastChan, int intVec)
{
    ipac_idProm_t *id;
    unsigned char manufacturer;
    unsigned char model;
    drvIp330Pvt *pPvt;
    asynStatus status;
    int i;
    epicsTimerQueueId timerQueueId;

    pPvt = callocMustSucceed(1, sizeof(*pPvt), "initIp330");
    pPvt->portName = epicsStrDup(portName);
    pPvt->carrier = carrier;
    pPvt->slot = slot;
    pPvt->firstChan = firstChan;
    pPvt->lastChan = lastChan;
    timerQueueId = epicsTimerQueueAllocate(1, epicsThreadPriorityLow);
    pPvt->timerId = epicsTimerQueueCreateTimer(timerQueueId, autoCalibrate,
                                               (void *)pPvt);

    if (ipmCheck(carrier, slot)) {
       errlogPrintf("initIp330: bad carrier or slot\n");
       return -1;
    }

    id = (ipac_idProm_t *) ipmBaseAddr(carrier, slot, ipac_addrID);
    manufacturer = id->manufacturerId & 0xff;
    model = id->modelId & 0xff;
    if(manufacturer!=ACROMAG_ID) {
        errlogPrintf("initIp330 manufacturer 0x%x not ACROMAG_ID\n",
                     manufacturer);
        return -1;
    }
    if(model!=ACRO_IP330) {
       errlogPrintf("initIp330 model 0x%x not a ACRO_IP330\n",model);
       return -1;
    }
    if(strcmp(typeString,"D")==0) {
        pPvt->type = differential;
    } else if(strcmp(typeString,"S")==0) {
        pPvt->type = singleEnded;
    } else {
        errlogPrintf("initIp330 illegal type. Must be \"D\" or \"S\"\n");
        return -1;
    }
    for(pPvt->range=0; pPvt->range<nRanges; pPvt->range++) {
        if(strcmp(rangeString,rangeName[pPvt->range])==0) break;
    }
    if(pPvt->range>=nRanges) {
        errlogPrintf("initIp330 illegal range\n");
        return -1;
    }

    pPvt->mailBoxOffset = 16;
    pPvt->chanSettings = callocMustSucceed(MAX_IP330_CHANNELS,
                                           sizeof(ip330ADCSettings),
                                           "initIp330");
    ellInit(&pPvt->clientList);

    /* Link with higher level routines */
    pPvt->common.interfaceType = asynCommonType;
    pPvt->common.pinterface  = (void *)&drvIp330Common;
    pPvt->common.drvPvt = pPvt;
    pPvt->int32.interfaceType = asynInt32Type;
    pPvt->int32.pinterface  = (void *)&drvIp330Int32;
    pPvt->int32.drvPvt = pPvt;
    pPvt->float64.interfaceType = asynFloat64Type;
    pPvt->float64.pinterface  = (void *)&drvIp330Float64;
    pPvt->float64.drvPvt = pPvt;
    pPvt->int32Callback.interfaceType = asynInt32CallbackType;
    pPvt->int32Callback.pinterface  = (void *)&drvIp330Int32Callback;
    pPvt->int32Callback.drvPvt = pPvt;
    pPvt->float64Callback.interfaceType = asynFloat64CallbackType;
    pPvt->float64Callback.pinterface  = (void *)&drvIp330Float64Callback;
    pPvt->float64Callback.drvPvt = pPvt;
    pPvt->int32ArrayCallback.interfaceType = asynInt32ArrayCallbackType;
    pPvt->int32ArrayCallback.pinterface  = (void *)&drvIp330Int32ArrayCallback;
    pPvt->int32ArrayCallback.drvPvt = pPvt;
    pPvt->ip330.interfaceType = asynIp330Type;
    pPvt->ip330.pinterface  = (void *)&drvIp330;
    pPvt->ip330.drvPvt = pPvt;
    status = pasynManager->registerPort(portName,
                                        ASYN_MULTIDEVICE, /*is multiDevice*/
                                        1,  /*  autoconnect */
                                        0,  /* medium priority */
                                        0); /* default stack size */
    if (status != asynSuccess) {
        errlogPrintf("initIp330 ERROR: Can't register port\n");
        return -1;
    }
    status = pasynManager->registerInterface(portName,&pPvt->common);
    if (status != asynSuccess) {
        errlogPrintf("initIp330 ERROR: Can't register common.\n");
        return -1;
    }
    status = pasynManager->registerInterface(pPvt->portName,&pPvt->int32);
    if (status != asynSuccess) {
        errlogPrintf("initIp330 ERROR: Can't register int32\n");
        return -1;
    }
    status = pasynManager->registerInterface(pPvt->portName,&pPvt->float64);
    if (status != asynSuccess) {
        errlogPrintf("initIp330 ERROR: Can't register float64\n");
        return -1;
    }
    status = pasynManager->registerInterface(pPvt->portName,
                                             &pPvt->int32Callback);
    if (status != asynSuccess) {
        errlogPrintf("initIp330 ERROR: Can't register int32Callback\n");
        return -1;
    }
    status = pasynManager->registerInterface(pPvt->portName,
                                             &pPvt->float64Callback);
    if (status != asynSuccess) {
        errlogPrintf("initIp330 ERROR: Can't register float64Callback\n");
        return -1;
    }
    status = pasynManager->registerInterface(pPvt->portName,
                                             &pPvt->int32ArrayCallback);
    if (status != asynSuccess) {
        errlogPrintf("initIp330 ERROR: Can't register int32ArrayCallback\n");
        return -1;
    }
    status = pasynManager->registerInterface(pPvt->portName,&pPvt->ip330);
    if (status != asynSuccess) {
        errlogPrintf("initIp330 ERROR: Can't register ip330.\n");
        return -1;
    }

    /* Create asynUser for debugging */
    pPvt->pasynUser = pasynManager->createAsynUser(0, 0);

    /* Connect to device */
    status = pasynManager->connectDevice(pPvt->pasynUser, portName, 0);
    if (status != asynSuccess) {
        errlogPrintf("initIp330, connectDevice failed for ip330\n");
        return -1;
    }

    /* Program device registers */
    pPvt->regs = (ip330ADCregs *) ipmBaseAddr(carrier, slot, ipac_addrIO);;
    /* wdId = new WatchDog; */
    pPvt->lock = epicsMutexCreate();
    pPvt->regs->startConvert = 0x0000;
    pPvt->regs->intVector = intVec;
    if(intConnect(INUM_TO_IVEC(intVec), (VOIDFUNCPTR)intFunc, 
                  (int)pPvt) == ERROR) {
        errlogPrintf("initIp330 intConnect Failure\n");
        return -1;
    }
    /* Reboot::rebootHookAdd(rebootCallback,(void *)this); */
    pPvt->regs->control = 0x0000;
    pPvt->regs->control |= 0x0002; /* Output Data Format = Straight Binary */
    setTrigger(pPvt, TRIGGER_DIRECTION);
    setScanMode(pPvt, SCAN_MODE);
    pPvt->regs->control |= 0x0800; /* Timer Enable = Enable */
    setScanPeriod(pPvt, pPvt->pasynUser, MICROSECONDS_PER_SCAN/1.e6);
    if(pPvt->type==differential) {
        pPvt->regs->control |= 0x0000;
    } else {
        pPvt->regs->control |= 0x0008;
    }
    /* Channels to convert */
    pPvt->regs->startChanVal = firstChan;
    pPvt->regs->endChanVal = lastChan;
    for (i = firstChan; i <= lastChan; i++) {
        /* default to gain of 0 */
        setGainPrivate(pPvt, pPvt->range, 0, i);
    }
    setSecondsBetweenCalibrate(pPvt, pPvt->pasynUser, SECONDS_BETWEEN_CALIBRATE);

    return 0;
}

int configIp330(const char *portName, scanModeType scanMode, 
                const char *triggerString, int microSecondsPerScan, 
                int secondsBetweenCalibrate)
{
    asynStatus status;
    asynInterface *pasynInterface;
    asynUser *pasynUser;
    drvIp330Pvt *pPvt;

    pasynUser = pasynManager->createAsynUser(0, 0);
    status = pasynManager->connectDevice(pasynUser, portName, 0);
    if (status != asynSuccess) {
        errlogPrintf("configIp330, error in connectDevice %s\n",
                     pasynUser->errorMessage);
        return -1;
    }
    pasynInterface = pasynManager->findInterface(pasynUser, asynIp330Type, 1);
    if (!pasynInterface) {
        errlogPrintf("initIp330 cannot find Ipi330 interface %s\n",
                     pasynUser->errorMessage);
        return -1;
    }
    pPvt = pasynInterface->drvPvt;

    config(pPvt, scanMode, triggerString, microSecondsPerScan/1.e6,
           secondsBetweenCalibrate);
    return(0);
}


static int config(drvIp330Pvt *pPvt, scanModeType scan, 
                  const char *triggerString, 
                  double secondsPerScan, int secondsCalibrate)
{
    int trigger;

    if (pPvt->rebooting) taskSuspend(0);
    for(trigger=0; trigger<nTriggers; trigger++) {
        if(strcmp(triggerString,triggerName[trigger])==0) break;
    }
    if(trigger>=nTriggers) {
        errlogPrintf("drvIp330::config illegal trigger\n");
        return(-1);
    }
    setTrigger(pPvt, (triggerType)trigger);
    setScanMode(pPvt, scan);
    setScanPeriod(pPvt, pPvt->pasynUser, secondsPerScan);
    setSecondsBetweenCalibrate(pPvt, pPvt->pasynUser, secondsCalibrate);
    autoCalibrate((void *)pPvt);
    pPvt->regs->control |= 0x2000; /* = Interrupt After All Selected */
    pPvt->intMsgQId = epicsMessageQueueCreate(MAX_MESSAGES, 
                                              MAX_IP330_CHANNELS*sizeof(int));
    if (epicsThreadCreate("Ip330intTask",
                           epicsThreadPriorityHigh, 10000,
                           (EPICSTHREADFUNC)intTask,
                           pPvt) == NULL)
       errlogPrintf("Ip330intTask epicsThreadCreate failure\n");
    return(0);
}

static asynStatus readInt32(void *drvPvt, asynUser *pasynUser, 
                            epicsInt32 *value)
{
    drvIp330Pvt *pPvt = (drvIp330Pvt *)drvPvt;
    int channel;

    if (pPvt->rebooting) taskSuspend(0);
    pasynManager->getAddr(pasynUser, &channel);
    *value = pPvt->correctedData[channel];
    return(asynSuccess);
}

static asynStatus readFloat64(void *drvPvt, asynUser *pasynUser,
                              epicsFloat64 *value)
{
    int ivalue;
    asynStatus status;

    status = readInt32(drvPvt, pasynUser, &ivalue);
    *value = (double)ivalue;
    return(status);
}

static asynStatus writeInt32(void *drvPvt, asynUser *pasynUser, 
                             epicsInt32 value)
{
    asynPrint(pasynUser, ASYN_TRACE_ERROR,
              "drvIp330::writeInt32 invalid command\n");
    return(asynError);
}

static asynStatus writeFloat64(void *drvPvt, asynUser *pasynUser, 
                               epicsFloat64 value)
{
    asynPrint(pasynUser, ASYN_TRACE_ERROR,
              "drvIp330::writeFloat64 invalid command\n");
    return(asynError);
}

static void correctAll(drvIp330Pvt *pPvt)
{
    int value, raw;
    int i;

    if (pPvt->rebooting) taskSuspend(0);
    if (pPvt->secondsBetweenCalibrate < 0) {
        for (i=0; i<MAX_IP330_CHANNELS; i++) {
           pPvt->correctedData[i] = pPvt->chanData[i];
        }
    } else {
        epicsMutexLock(pPvt->lock);
        for (i=0; i<MAX_IP330_CHANNELS; i++) {
           raw = pPvt->chanData[i];
           value = (int) (pPvt->chanSettings[i].adj_slope *
                   (((double)raw + pPvt->chanSettings[i].adj_offset)));
           pPvt->correctedData[i] = value;
        }
        epicsMutexUnlock(pPvt->lock);
    }
}

static asynStatus setGain(void *drvPvt, asynUser *pasynUser, 
                          int gain)
{
    drvIp330Pvt *pPvt = (drvIp330Pvt *)drvPvt;
    int status = 0;
    int channel;

    pasynManager->getAddr(pasynUser, &channel);

    if (pPvt->rebooting) taskSuspend(0);
    if(gain<0 || gain>nGains || channel<0 || channel>pPvt->lastChan) 
       return(-1);
    if(gain != pPvt->chanSettings[channel].gain) 
       status = setGainPrivate(pPvt, pPvt->range, gain, channel);
    return(status);
}

static int setGainPrivate(drvIp330Pvt *pPvt, int range, int gain, int channel)
{
    unsigned short saveControl;

    if (pPvt->rebooting) taskSuspend(0);
    if(gain<0 || gain>=nGains) {
        asynPrint(pPvt->pasynUser, ASYN_TRACE_ERROR,
                  "drvIp330::setGainPrivate illegal gain value %d\n", gain);
        return(-1);
    }
    saveControl = pPvt->regs->control;
    pPvt->regs->control &= SCAN_DISABLE;
    pPvt->chanSettings[channel].gain = gain;
    pPvt->chanSettings[channel].volt_callo = 
                                calibrationSettings[range][gain].volt_callo;
    pPvt->chanSettings[channel].volt_calhi = 
                                calibrationSettings[range][gain].volt_calhi;
    pPvt->chanSettings[channel].ctl_callo = 
                                calibrationSettings[range][gain].ctl_callo;
    pPvt->chanSettings[channel].ctl_calhi = 
                                calibrationSettings[range][gain].ctl_calhi;
    pPvt->chanSettings[channel].ideal_span = 
                                calibrationSettings[range][gain].ideal_span;
    pPvt->chanSettings[channel].ideal_zero = 
                                calibrationSettings[range][gain].ideal_zero;
    pPvt->regs->gain[channel] = gain;
    pPvt->regs->control = saveControl;
    calibrate(pPvt, channel);
    return(0);
}

static int setTrigger(drvIp330Pvt *pPvt, triggerType trig)
{
    if (pPvt->rebooting) taskSuspend(0);
    if (trig < 0 || trig >= nTriggers) return(-1);
    pPvt->trigger = trig;
    if (pPvt->trigger == output) pPvt->regs->control |= 0x0004;
    return(0);
}

static int setScanMode(drvIp330Pvt *pPvt, scanModeType mode)
{
    if (pPvt->rebooting) taskSuspend(0);
    if ((mode < disable) || (mode > convertOnExternalTriggerOnly)) return(-1);
    pPvt->scanMode = mode;
    pPvt->regs->control &= ~(0x7 << 8);      /* Kill all control bits first */
    pPvt->regs->control |= pPvt->scanMode << 8;
    return(0);
}


static asynStatus registerInt32Callback(void *drvPvt, asynUser *pasynUser,
                                        asynInt32Clbk callback, void *pvt)
{
    return(registerCallback(drvPvt, pasynUser, callback, pvt, typeInt32));
}

static asynStatus registerFloat64Callback(void *drvPvt, asynUser *pasynUser,
                                          asynFloat64Clbk callback, void *pvt)
{
    return(registerCallback(drvPvt, pasynUser, callback, pvt, typeFloat64));
}

static asynStatus registerInt32ArrayCallback(void *drvPvt, asynUser *pasynUser,
                                         asynInt32ArrayClbk callback, void *pvt)
{
    return(registerCallback(drvPvt, pasynUser, callback, pvt, typeInt32Array));
}

static asynStatus registerCallback(void *drvPvt, asynUser *pasynUser,
                                   void *callback, void *pvt, dataType dataType)
{
    drvIp330Pvt *pPvt = (drvIp330Pvt *)drvPvt;
    ip330Client *pClient = callocMustSucceed(1, sizeof(*pClient),
                                              "drvIp330::registerCallback");

    pClient->dataType = dataType;
    pasynManager->getAddr(pasynUser, &pClient->channel);
    switch(dataType) {
    case typeInt32:
        pClient->int32Callback = callback;
        break;
    case typeFloat64:
        pClient->float64Callback = callback;
        break;
    case typeInt32Array:
        pClient->int32ArrayCallback = callback;
        break;
    }
    pClient->pvt = pvt;
    ellAdd(&pPvt->clientList, (ELLNODE *)pClient);
    return(asynSuccess);
}

       
static void intFunc(void *drvPvt)
{
    drvIp330Pvt *pPvt = (drvIp330Pvt *)drvPvt;
    int i;
    int data[MAX_IP330_CHANNELS];

    if (pPvt->type == differential) {
       /* Must alternate between reading data from mailBox[i] and mailBox[i+16]
        * Except in case of uniform/burstSingle, where only half of mailbox is 
        * used. */
       if(pPvt->scanMode == uniformSingle || pPvt->scanMode==burstSingle 
                                          || pPvt->mailBoxOffset==16)
          pPvt->mailBoxOffset = 0; 
       else 
          pPvt->mailBoxOffset = 16;
    }
    for (i = pPvt->firstChan; i <= pPvt->lastChan; i++) {
        data[i] = (pPvt->regs->mailBox[i + pPvt->mailBoxOffset]);
    }
    /* Wake up task which calls callback routines */
    if (epicsMessageQueueTrySend(pPvt->intMsgQId, data, sizeof(data)) == 0)
        pPvt->messagesSent++;
    else
        pPvt->messagesFailed++;
    if(!pPvt->rebooting) ipmIrqCmd(pPvt->carrier, pPvt->slot, 0, ipac_irqEnable);
}

static void intTask(drvIp330Pvt *pPvt)
{
    int    i;
    int data[MAX_IP330_CHANNELS];
    ip330Client *pClient;

    while(1) {
       /* Wait for event from interrupt routine */
       epicsMessageQueueReceive(pPvt->intMsgQId, data, sizeof(data));
       for (i=pPvt->firstChan; i<=pPvt->lastChan; i++) {
           pPvt->chanData[i] = data[i];
       }
       /* Correct the data */
       correctAll(pPvt);
                 
       /* Call the callback routines which have registered */
       pClient = (ip330Client *)ellFirst(&pPvt->clientList);
       while(pClient) {
           switch(pClient->dataType) {
           case typeInt32:
               pClient->int32Callback(pClient->pvt,
                                      pPvt->correctedData[pClient->channel]);
               break;
           case typeFloat64:
               pClient->float64Callback(pClient->pvt,
                                (double)pPvt->correctedData[pClient->channel]);
               break;
           case typeInt32Array:
               pClient->int32ArrayCallback(pClient->pvt,
                                           pPvt->correctedData);
               break;
           }
           pClient = (ip330Client *)ellNext(pClient);
       }
   }
}


#define MAX_TIMES 1000000
static void waitNewData(drvIp330Pvt *pPvt)
{
    int ntimes=0;
    while(ntimes++ < MAX_TIMES) {
        if (pPvt->regs->newData[1]==0xffff) return;
    }
    errlogPrintf("drvIp330::waitNewData time out\n");
}

static asynStatus setSecondsBetweenCalibrate(void *drvPvt, asynUser *pasynUser,
                                             double seconds)
{
    drvIp330Pvt *pPvt = (drvIp330Pvt *)drvPvt;

    if (pPvt->rebooting) taskSuspend(0);
    pPvt->secondsBetweenCalibrate = seconds;
    autoCalibrate((void *)pPvt);
    return(asynSuccess);
}

static void autoCalibrate(void *drvPvt)
{
    drvIp330Pvt *pPvt = (drvIp330Pvt *)drvPvt;
    int i;

    if (pPvt->rebooting) taskSuspend(0);
    epicsTimerCancel(pPvt->timerId);
    if (pPvt->secondsBetweenCalibrate < 0) return;
    asynPrint(pPvt->pasynUser, ASYN_TRACE_FLOW,
              "drvIp330::autoCalibrate starting calibration\n");
    for(i=pPvt->firstChan; i<= pPvt->lastChan; i++)
        calibrate(pPvt, i);
    if (pPvt->secondsBetweenCalibrate != 0)
        epicsTimerStartDelay(pPvt->timerId, pPvt->secondsBetweenCalibrate);
}


/* See Acromag User's Manual for details about callibration */
static int calibrate(drvIp330Pvt *pPvt, int channel)
{
    unsigned short saveControl;
    unsigned char saveStartChanVal;
    unsigned char saveEndChanVal;
    unsigned short val;
    double count_callo;
    double count_calhi;
    double m, cal1, cal2;
    long sum;
    int i;

    if (pPvt->rebooting) taskSuspend(0);
    saveControl = pPvt->regs->control;
    pPvt->regs->control &= SCAN_DISABLE;
    /* Disable scan mode and interrupts */
    /* determine count_callo */
    saveStartChanVal = pPvt->regs->startChanVal;
    saveEndChanVal = pPvt->regs->endChanVal;
    pPvt->regs->endChanVal = 0x1F;
    pPvt->regs->startChanVal = 0x00;
    for (i = 0; i < MAX_IP330_CHANNELS; i++) 
        pPvt->regs->gain[i] = pPvt->chanSettings[channel].gain;
    pPvt->regs->control = 0x0402 | (0x0038 & 
                                    (pPvt->chanSettings[channel].ctl_callo));
    pPvt->regs->startConvert = 0x0001;
    waitNewData(pPvt);
    /* Ignore first set of data so that adc has time to settle */
    pPvt->regs->startConvert = 0x0001;
    waitNewData(pPvt);
    asynPrint(pPvt->pasynUser, ASYN_TRACE_FLOW,
              "drvIp330::calibrate. Raw values low\n");
    sum = 0;
    for (i = 0; i < MAX_IP330_CHANNELS; i++) {
        val = pPvt->regs->mailBox[i];
        sum = sum + val;
        asynPrint(pPvt->pasynUser, ASYN_TRACE_FLOW,
                  "%hu ",val);
        if((i+1)%8 == 0) asynPrint(pPvt->pasynUser, ASYN_TRACE_FLOW, "\n");
    }
    count_callo = ((double)sum)/(double)MAX_IP330_CHANNELS;
    /* determine count_calhi */
    pPvt->regs->control = 0x0402 | (0x0038 & 
                                   (pPvt->chanSettings[channel].ctl_calhi));
    pPvt->regs->startConvert = 0x0001;
    waitNewData(pPvt);
    /* Ignore first set of data so that adc has time to settle */
    pPvt->regs->startConvert = 0x0001;
    waitNewData(pPvt);
    asynPrint(pPvt->pasynUser, ASYN_TRACE_FLOW,
              "drvIp33::calibrate. Raw values high\n");
    sum = 0;
    for (i = 0; i < MAX_IP330_CHANNELS; i++) {
        val = pPvt->regs->mailBox[i];
        sum = sum + val;
        asynPrint(pPvt->pasynUser, ASYN_TRACE_FLOW,
                  "%hu ",val);
        if((i+1)%8 == 0) asynPrint(pPvt->pasynUser, ASYN_TRACE_FLOW, "\n");
    }
    count_calhi = ((double)sum)/(double)MAX_IP330_CHANNELS;

    m = pgaGain[pPvt->chanSettings[channel].gain] *
        ((pPvt->chanSettings[channel].volt_calhi - 
          pPvt->chanSettings[channel].volt_callo) /
         (count_calhi - count_callo));
    cal1 = (65536.0 * m) / pPvt->chanSettings[channel].ideal_span;
    cal2 =
          ((pPvt->chanSettings[channel].volt_callo * 
            pgaGain[pPvt->chanSettings[channel].gain])
            - pPvt->chanSettings[channel].ideal_zero)
          / m - count_callo;
    epicsMutexLock(pPvt->lock);
    pPvt->chanSettings[channel].adj_slope = cal1;
    pPvt->chanSettings[channel].adj_offset = cal2;
    epicsMutexUnlock(pPvt->lock);
    asynPrint(pPvt->pasynUser, ASYN_TRACE_FLOW,
              "drvIp330::calibrate channel %d adj_slope %e adj_offset %e\n",
              channel, cal1, cal2);
    /* restore control and gain values */
    pPvt->regs->control &= SCAN_DISABLE;
    pPvt->regs->control = saveControl;
    /* Restore pre - calibrate control register state */
    pPvt->regs->startChanVal = saveStartChanVal;
    pPvt->regs->endChanVal = saveEndChanVal;
    for (i = 0; i < MAX_IP330_CHANNELS; i++) 
        pPvt->regs->gain[i] = pPvt->chanSettings[i].gain;
    pPvt->mailBoxOffset=16; /* make it start over*/
    if (!pPvt->rebooting) ipmIrqCmd(pPvt->carrier, pPvt->slot, 
                                    0, ipac_irqEnable);
    pPvt->regs->startConvert = 0x0001;
    return (0);
}

static void rebootCallback(void *drvPvt)
{
    drvIp330Pvt *pPvt = (drvIp330Pvt *)drvPvt;
    pPvt->regs->control &= SCAN_DISABLE;
    pPvt->rebooting = 1;
}

static double getActualScanPeriod(drvIp330Pvt *pPvt)
{
    double microSeconds;

    if (pPvt->rebooting) taskSuspend(0);
    microSeconds = (15. * (pPvt->lastChan - pPvt->firstChan + 1)) +
          (pPvt->regs->timePrescale * pPvt->regs->conversionTime) / 8.;
    return(microSeconds / 1.e6);
}

/* Note: we cache actualScanPeriod for efficiency, so that
 * getActualScanPeriod is only called when the time registers are
 * changed.  It is important for getScanPeriod to be efficient, since
 * it is often called from the interrupt routines of servers which use Ip330. */

static double getScanPeriod(void *drvPvt, asynUser *pasynUser)
{
    drvIp330Pvt *pPvt = (drvIp330Pvt *)drvPvt;

    if (pPvt->rebooting) taskSuspend(0);
    return (pPvt->actualScanPeriod);
}

static double setScanPeriod(void *drvPvt, asynUser *pasynUser, 
                            double seconds)
{
    drvIp330Pvt *pPvt = (drvIp330Pvt *)drvPvt;
    double microSeconds = seconds * 1.e6;
    int timePrescale;
    int timeConvert;
    int status=0;
    double delayTime;

    if (pPvt->rebooting) taskSuspend(0);
    /* This function computes the optimal values for the prescale and
     * conversion timer registers.  microSecondsPerScan is the time
     * in microseconds between successive scans.
     * The delay time includes the 15 microseconds required to convert each 
     * channel. */
    if (pPvt->scanMode == burstContinuous) 
        delayTime = microSeconds - 15. * (pPvt->lastChan - pPvt->firstChan+1);
    else if (pPvt->scanMode == uniformContinuous) 
        delayTime = microSeconds / (pPvt->lastChan - pPvt->firstChan + 1);
    else
        delayTime = microSeconds;
    for (timePrescale=64; timePrescale<=255; timePrescale++) {
        timeConvert = (int) ((8. * delayTime)/(double)timePrescale + 0.5);
        if (timeConvert < 1) {
            errlogPrintf("drvIp330::setScanPeriod, time interval too short\n");
            timeConvert = 1;
            status=-1;
            goto finish;
        }
        if (timeConvert <= 65535) goto finish;
    }
    errlogPrintf("drvIp330::setScanPeriod, time interval too long\n");
    timeConvert = 65535;
    timePrescale = 255;
    status=-1;
finish:
    pPvt->regs->timePrescale = timePrescale;
    pPvt->regs->conversionTime = timeConvert;
    pPvt->actualScanPeriod = getActualScanPeriod(pPvt);
    asynPrint(pPvt->pasynUser, ASYN_TRACE_FLOW,
              "drvIp330::setScanPeriod, requested time=%f\n" 
              "   prescale=%d, convert=%d, actual=%f\n",
              seconds, timePrescale, timeConvert, pPvt->actualScanPeriod);
    if (status == 0) return(pPvt->actualScanPeriod); else return(-1.);
}

/* asynCommon routines */

/* Report  parameters */
static void report(void *drvPvt, FILE *fp, int details)
{
    drvIp330Pvt *pPvt = (drvIp330Pvt *)drvPvt;
    int i;

    fprintf(fp, "Port: %s, carrier %d slots %d, base address=%p\n", 
            pPvt->portName, pPvt->carrier, pPvt->slot, pPvt->regs);
    if (details >= 1) {
        fprintf(fp, "    messages sent OK=%d; send failed (queue full)=%d\n",
                pPvt->messagesSent, pPvt->messagesFailed);
        fprintf(fp, "    firstChan=%d, lastChan=%d, scanPeriod=%f\n",
                pPvt->firstChan, pPvt->lastChan, pPvt->actualScanPeriod);
        for (i=0; i<MAX_IP330_CHANNELS; i++) {
           fprintf(fp, "    chan %d, offset=%f slope=%f, raw=%d corrected=%d\n",
                   i, pPvt->chanSettings[i].adj_offset, 
                   pPvt->chanSettings[i].adj_slope, 
                   pPvt->chanData[i], pPvt->correctedData[i]);
        }
    }
}

/* Connect */
static asynStatus connect(void *drvPvt, asynUser *pasynUser)
{
    pasynManager->exceptionConnect(pasynUser);
    return(asynSuccess);
}

/* Disconnect */
static asynStatus disconnect(void *drvPvt, asynUser *pasynUser)
{
    pasynManager->exceptionDisconnect(pasynUser);
    return(asynSuccess);
}

static const iocshArg initArg0 = { "portName",iocshArgString};
static const iocshArg initArg1 = { "Carrier",iocshArgInt};
static const iocshArg initArg2 = { "Slot",iocshArgInt};
static const iocshArg initArg3 = { "typeString",iocshArgString};
static const iocshArg initArg4 = { "rangeString",iocshArgString};
static const iocshArg initArg5 = { "firstChan",iocshArgInt};
static const iocshArg initArg6 = { "lastChan",iocshArgInt};
static const iocshArg initArg7 = { "intVec",iocshArgInt};
static const iocshArg * const initArgs[8] = {&initArg0,
                                             &initArg1,
                                             &initArg2,
                                             &initArg3,
                                             &initArg4,
                                             &initArg5,
                                             &initArg6,
                                             &initArg7};
static const iocshFuncDef initFuncDef = {"initIp330",8,initArgs};
static void initCallFunc(const iocshArgBuf *args)
{
    initIp330(args[0].sval, args[1].ival, args[2].ival,
              args[3].sval, args[4].sval, args[5].ival,
              args[6].ival, args[7].ival);
}

static const iocshArg configArg0 = { "portName",iocshArgString};
static const iocshArg configArg1 = { "scanMode",iocshArgInt};
static const iocshArg configArg2 = { "triggerString",iocshArgString};
static const iocshArg configArg3 = { "microSecondsPerScan",iocshArgInt};
static const iocshArg configArg4 = { "secondsBetweenCalibrate",iocshArgInt};
static const iocshArg * configArgs[5] = {&configArg0,
                                         &configArg1,
                                         &configArg2,
                                         &configArg3,
                                         &configArg4};
static const iocshFuncDef configFuncDef = {"configIp330",9,configArgs};
static void configCallFunc(const iocshArgBuf *args)
{
    configIp330(args[0].sval, (scanModeType)args[1].ival, args[2].sval,
                args[3].ival, args[4].ival);
}

void ip330Register(void)
{
    iocshRegister(&initFuncDef,initCallFunc);
    iocshRegister(&configFuncDef,configCallFunc);
}

epicsExportRegistrar(ip330Register);