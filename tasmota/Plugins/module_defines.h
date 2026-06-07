
typedef struct {
  bool          grpflg;
  bool          usridx;
  uint16_t      command_code;
  uint32_t      index;
  uint32_t      data_len;
  int32_t       payload;
  char         *topic;
  char         *data;
  char         *command;
} XdrvMailbox;

extern void AddLog(uint32_t loglevel, PGM_P formatP, ...);

#ifndef MAX_MOD_STORES
#define MAX_MOD_STORES 4
#endif

// this descriptor is in flash so only 32 bit access allowed
#pragma pack(4)
typedef struct {
  MD_TYPE sync;
  MD_TYPE arch;
  MD_TYPE type;
  MD_TYPE revision;
  char name[16];
  // 32 => 0x20
  int32_t (*mod_func_execute)(uint32_t);
  void (*end_of_module)(void);
  MD_TYPE size;
  // 40 => 0x28
  MD_TYPE execution_offset;
  // 44 => 0x2c
  MD_TYPE mtv;
  MD_TYPE jtab;
  // 52 = 0x34
  uint32_t mod_start_org;
  int32_t (*mod_func_execute_org)(uint32_t);
  
  // 56
#ifdef ESP8266
  MODULE_STORE ms[MAX_MOD_STORES];
#else
  MODULE_STORE ms[];
#endif
} FLASH_MODULE;


#undef vsnprintf_P 
#undef br_aes_small_ctr_init
#undef br_gcm_init
#undef br_gcm_reset
#undef br_gcm_aad_inject
#undef br_gcm_flip
#undef br_gcm_run
#undef br_gcm_check_tag_trunc

#define EXEC_OFFSET ((FLASH_MODULE*)mt->mod_addr)->execution_offset

#ifdef ESP8266
#define portMUX_TYPE void
#endif

// vector table calls
#define jWire                           ( TwoWire*)                                    jt[0]
#define jWire1                          ( TwoWire*)                                    jt[1]
#define jSerial                         ( HardwareSerial*)                             jt[2]
#define jI2cSetDevice(ADDR,BUS)         (( bool (*)(uint32_t,uint32_t) )               jt[3])(ADDR,BUS)
#define jI2cSetActiveFound(A,B,C)       (( void (*)(uint32_t,const char *, uint32_t) ) jt[4])(A,B,C)
#define jAddLog(A,...)                  (( void (*)(uint32_t,const char *, ...) )      jt[5])(A,##__VA_ARGS__)
#define jResponseAppend_P(A,...)        (( void (*)(const char *, ...) )               jt[6])(A,##__VA_ARGS__)
#define jWSContentSend_PD(A,...)        (( void (*)(const char *, ...) )               jt[7])(A,##__VA_ARGS__)
#define jftostrfd(A,B,C)                (( char *(*)(float, uint8_t, char*) )          jt[8])(A,B,C)
#define jcalloc(A,B)                    (( void *(*)(size_t, size_t) )                 jt[9])(A,B)
// 10
#define jfscale(A,B,C)                  (( float (*)(int32_t, float, float) )          jt[10])(A,B,C)
#define sprint(A)                       (( void (*)(const char*) )                     jt[11])(A)
#define jbeginTransmission(BUS,ADDR)    (( void (*)(TwoWire*,uint8_t) )                jt[12])(BUS,ADDR)
#define jwrite(BUS,VAL)                 (( size_t (*)(TwoWire*,uint8_t) )              jt[13])(BUS,VAL)
#define jendTransmission(BUS,VAL)       (( uint8_t (*)(TwoWire*,uint8_t) )             jt[14])(BUS,VAL)
#define jrequestFrom(BUS,ADDR,NUM)      (( size_t (*)(TwoWire*,uint8_t,size_t) )       jt[15])(BUS,ADDR,NUM)
#define jread(BUS)                      (( int (*)(TwoWire*) )                         jt[16])(BUS)
#define fshowhex(VAL)                   (( void (*)(uint32_t) )                        jt[17])(VAL)
#define jfree(MEM)                      (( void (*)(void*) )                           jt[18])(MEM)
#define jI2cWrite16(ADDR,REG,VAL,BUS)   (( bool (*)(uint8_t, uint8_t, uint16_t, uint8_t) )      jt[19])(ADDR,REG,VAL,BUS)
// 20
#define jI2cRead16(ADDR,REG,BUS)        (( uint16_t (*)(uint8_t, uint8_t, uint8_t) )  jt[20])(ADDR,REG,BUS)
#define jI2cValidRead16(DATA,ADDR,REG,BUS)  (( bool (*)(uint16_t *,uint8_t,uint8_t,uint8_t) )   jt[21])(DATA,ADDR,REG,BUS)
#define jsnprintf_P(A,B,C,...)          (( void (*)(char *,size_t,const char *,...) )  jt[22])(A,B,C,##__VA_ARGS__)
#define jXdrvRulesProcess(A)            (( bool (*)(bool) )                            jt[23])(A)
#define jResponseJsonEnd                (( void (*)(void) )                            jt[24])
#define jdelay(A)                       (( void (*)(uint32_t) )                        jt[25])(A)
#define jI2cActive(A)                   (( bool (*)(uint32_t) )                        jt[26])(A)
#define jResponseJsonEndEnd             (( void (*)(void) )                            jt[27])
#define jIndexSeparator                 (( char (*)(void) )                            jt[28])
#define jResponse_P(A,...)                (( int (*)(const char * formatP, ...) )      jt[29])(A,##__VA_ARGS__)
// 30
#define jI2cResetActive(REG,BUS)        (( void (*)(uint32_t, uint32_t) )              jt[30])(REG,BUS)
#define jisnan(FVAL)                    (( bool (*)(float) )                           jt[31])(FVAL)
#define jConvertTemp(FVAL)              (( float (*)(float) )                          jt[32])(FVAL)
#define jConvertHumidity(FVAL)          (( float (*)(float) )                          jt[33])(FVAL)
#define jTempHumDewShow(JSON,PASS,TYPES,TEMP,HUM)(( bool (*)(bool,bool,const char *,float,float) ) jt[34])(JSON,PASS,TYPES,TEMP,HUM)
#define jstrlcpy(DST,SRC,SIZE)          (( size_t (*)(char *,const char *,size_t) )                jt[35])(DST,SRC,SIZE)
#define jGetTextIndexed(DST,DSIZE,INDEX,HSTCK)(( char *(*)(char*,size_t,uint32_t,const char*) )    jt[36])(DST,DSIZE,INDEX,HSTCK)
#define JGetTasmotaGlobal(SEL)          ((uint32_t (*)(uint32_t) )                     jt[37])(SEL)
#define jiseq(FVAL)                     (( bool (*)(float) )                           jt[38])(FVAL)
// NOTE: these float jumptable macros are UNCHANGED for USE_PLUGIN_FLOAT_BITS.
// That flag is firmware-side only (the jt[] targets become bit-int wrappers).
// A soft-float _32r plugin's `float(float,float)` call already passes operands
// in a0/a1 and reads the result from a0 under the ilp32 ABI — which is exactly
// what the firmware bit-wrappers read/return — so an existing _32r binary runs
// unchanged on a flag-ON P4 firmware. See xdrv_123_plugins.ino.
#define jfdiv(P1,P2)                    (( float (*)(float,float) )                    jt[39])(P1,P2)
// 40
#define jfmul(P1,P2)                    (( float (*)(float,float) )                    jt[40])(P1,P2)
#define jfdiff(P1,P2)                   (( float (*)(float,float) )                    jt[41])(P1,P2)
#define jtofloat(P1)                    (( float (*)(uint64_t) )                       jt[42])(P1)
#define jfadd(P1,P2)                    (( float (*)(float,float) )                    jt[43])(P1,P2)
#define jI2cRead8(ADDR,REG)             (( uint8_t (*)(uint8_t,uint8_t) )              jt[44])(ADDR,REG)
#define jI2cWrite8(ADDR,REG,VAL)        (( bool (*)(uint8_t,uint8_t,uint8_t) )         jt[45])(ADDR,REG,VAL)
#define javailable(WIRE)                (( uint8_t (*)(TwoWire*) )                     jt[46])(WIRE)
#define jAddLogMissed(SENS,MISS)        (( void (*)(const char*,uint32_t) )            jt[47])(SENS,MISS)
#define jNAN                            (( float (*)(void) )                           jt[48])()
#define jgtsf2(P1,P2)                   (( int (*)(float,float) )                     jt[49])(P1,P2)
// 50
#define jltsf2(P1,P2)                   (( bool (*)(float,float) )                     jt[50])(P1,P2)
#define jeqsf2(P1,P2)                   (( bool (*)(float,float) )                     jt[51])(P1,P2)
#define jPin(PIN,INDEX)                 (( int (*)(uint32_t,uint32_t) )                jt[52])(PIN,INDEX)
#define jnewTS(RPIN,TPIN)               (( void* (*)(int32_t,int32_t) )                jt[53])(RPIN,TPIN)
#define jwriteTS(TSER,BUF,SIZE)         (( size_t (*)(void*,uint8_t*,uint32_t) )       jt[54])(TSER,BUF,SIZE)
#define jflushTS(TSER)                  (( void (*)(void*) )                           jt[55])(TSER)
#define jbeginTS(TSER,BAUD)             (( int (*)(void*,uint32_t) )                   jt[56])(TSER,BAUD)
#define jXdrvMailbox                    ((XdrvMailbox*)                                 jt[57])
#define jGetCommandCode(DST,DSIZE,NEEDLE,HSTCK)(( int (*)(char*,size_t,const char*,const char*) )    jt[58])(DST,DSIZE,NEEDLE,HSTCK)
#define jstrlen(STR)                    (( uint32_t (*)(char*) )                       jt[59])(STR)
// 60
#define jstrncasecmp_P(S1,S2,SIZE)      (( int (*)(const char*,const char *, size_t) ) jt[60])(S1,S2,SIZE)
#define jtoupper(CHAR)                  (( int (*)( int c ) )                          jt[61])(CHAR)
#define jiscale(A,B,C)                  (( int32_t (*)(int32_t, int32_t, int32_t) )    jt[62])(A,B,C)
#define jdeleteTS(TSER)                 (( void (*)(void*) )                           jt[63])(TSER)
#define jreadTS(TSER,BUF,SIZE)          (( size_t (*)(void*,uint8_t*,uint32_t) )       jt[64])(TSER,BUF,SIZE)
#define jread1TS(TSER)                  (( int (*)(void*) )                            jt[65])(TSER)
#define javailTS(TSER)                  (( uint8_t (*)(void*) )                        jt[66])(TSER)
#define jMqttPublishTeleSensor          (( void (*)(void) )                            jt[67])
#define jstrtoul(A,B,C)                 (( uint32_t (*)(const char *,char **, int) )   jt[68])(A,B,C)
#define jAddLogBuffer(A,B,C)            (( void (*)(uint32_t,uint8_t*, uint32_t) )     jt[69])(A,B,C)
// 70
#define jResponseTime_P(A,...)            (( int (*)(const char*, ...) )                 jt[70])(A,##__VA_ARGS__)
#define jClaimSerial                    (( void (*)(void) )                            jt[71])
#define jhardwareSerial(TSER)           (( bool (*)(void*) )                           jt[72])(TSER)
#define jmillis                         (( uint32_t (*)(void) )                        jt[73])
#define jsprintf_P(A,B,...)             (( void (*)(char*,const char * formatP,... ) ) jt[74])(A,B,##__VA_ARGS__)
#define jAddlogT(TXT)                   (( void (*)(char*) )                           jt[75])(TXT)
#define jtmod__divsi3(A,B)              (( int32_t (*)(int32_t,int32_t) )              jt[76])(A,B)
#define jtmod__udivsi3(A,B)             (( uint32_t (*)(uint32_t,uint32_t) )           jt[77])(A,B)
#define jtmod__floatsisf(A)             (( float (*)(int32_t) )                        jt[78])(A)
#define jtmod__floatunsisf(A)           (( float (*)(uint32_t) )                       jt[79])(A)
// 80
#define jFastPrecisePowf(A,B)           (( float (*)(float, float) )                   jt[80])(A,B)
#define JGetTasmotaGf(SEL)              (( float (*)(uint32_t) )                       jt[81])(SEL)
#define jtmod__muldi3(A,B)              (( int64_t (*)(int64_t,int64_t) )              jt[82])(A,B)
#define jtmod__fixunssfsi(A)            (( uint32_t (*)(float) )                       jt[83])(A)
#define jtmod__umodsi3(A,B)             (( uint32_t (*)(uint32_t,uint32_t) )           jt[84])(A,B)
#define jtwi_readFrom(A,B,C,D)(( unsigned char (*)(uint8_t,uint8_t*,unsigned int,uint8_t) ) jt[85])(A,B,C,D)
#ifdef ESP8266
#define jDecodeCommand(A,B,C)           (( bool (*)(const char*, void (* const x[])(void),MODULES_TABLE* )) jt[86])(A,B,C)
#else
#define jDecodeCommand(A,B,C)           (( bool (*)(const char*, void (* const x[])(void),volatile MODULES_TABLE* )) jt[86])(A,B,C)
#endif

#define jResponseCmndDone               (( void (*)(void) )                            jt[87])
#define jbwriteTS(TSER,VAL)             (( size_t (*)(void*,uint8_t) )                 jt[88])(TSER,VAL)
#define jmemcmp(A,B,SIZE)               (( int (*)(const void*,const void*,int) )      jt[89])(A,B,SIZE)
#define jToHex_P(A,B,C,D,E)             (( char* (*)(const unsigned char *, size_t, char *, size_t, char) ) jt[90])(A,B,C,D,E)
#define jmemset(A,B,C)                  (( void* (*)(void *,int,size_t) )              jt[91])(A,B,C)
#define jmemmove(A,B,C)                 (( void* (*)(void *,const void *,size_t) )     jt[92])(A,B,C)
#define jResponseCmndNumber(A)          (( void (*)(int))                              jt[93])(A)
#define jResponseCmndFloat(A,B)         (( void (*)(float,uint32_t))                   jt[94])(A,B)
#define jResponseAppendTHD(A,B)         (( int (*)(float,float))                       jt[95])(A,B)
#define jWSContentSend_THD(A,B,C)       (( void (*)(const char *,float,float))         jt[96])(A,B,C)
#define jstrncpy(A,B,C)                 (( char *(*)(char *, const char *, size_t) )   jt[97])(A,B,C)   
#define jisprint(A)                     (( int (*)(int) )                              jt[98])(A)
#define jisinf(A)                       (( bool (*)(float) )                           jt[99])(A)
#define jcopyStr(A)                     (( char *(*)(const char *) )                   jt[100])(A)
#define jsetClockStretchLimit(BUS,A)    (( void (*)(TwoWire*,uint32_t) )               jt[101])(BUS,A)
#define jwriten(BUS,BUF,LEN)            (( void (*)(TwoWire*,uint8_t*,uint32_t) )      jt[102])(BUS,BUF,LEN)
#define jmodff(A,B)                     (( float (*)(float,float*) )                   jt[103])(A,B)                     
#define jfl_const(A,B)                  (( float (*)(int32_t,int32_t) )                jt[104])(A,B) 
#define jWSContentSend_Temp(A,B)        (( void (*)(const char *, float) )             jt[105])(A,B)
#define jdelayMicroseconds(A)           (( void (*)(uint32_t) )                        jt[106])(A)
#define jdigitalRead(A)                 (( int (*)(uint8_t) )                          jt[107])(A)
#define jdigitalWrite(A,B)              (( void (*)(uint8_t, uint8_t) )                jt[108])(A,B)
#define jpinMode(A,B)                   (( void (*)(uint8_t, uint8_t) )                jt[109])(A,B)
#define jstrchr(A,B)                    (( char *(*)(char *, char) )                   jt[110])(A,B)
#define jtrimm(A)                       (( char *(*)(char *) )                         jt[111])(A)
#define jvTaskEnterCritical(A)          (( void (*)(portMUX_TYPE *) )                  jt[112])(A)
#define jvTaskExitCritical(A)           (( void (*)(portMUX_TYPE *) )                  jt[113])(A)
#define jdirectRead(A)                  (( uint32_t (*)(uint32_t) )                    jt[114])(A)
#define jdirectWriteLow(A)              (( void (*)(uint32_t) )                        jt[115])(A)
#define jdirectWriteHigh(A)             (( void (*)(uint32_t) )                        jt[116])(A)
#define jdirectModeInput(A)             (( void (*)(uint32_t) )                        jt[117])(A)
#define jdirectModeOutput(A)            (( void (*)(uint32_t) )                        jt[118])(A)
#define jCalcTempHumToAbsHum(A,B)       (( float (*)(float,float) )                    jt[119])(A,B)
#define jWSContentSend_P(A,...)         (( void (*)(const char *, ...) )               jt[120])(A,##__VA_ARGS__)
#define jHttpCheckPriviledgedAccess(A)  (( bool (*)(void) )                            jt[121])
#define jWSContentStart_P(A)            (( void (*)(const char *) )                    jt[122])(A)
#define jWSContentSendStyle             (( void (*)(void) )                            jt[123])
#define jWSContentSpaceButton(A,B)      (( void (*)(uint32_t,bool) )                   jt[124])(A,B)
#define jWSContentStop                  (( void (*)(void) )                            jt[125])
#define jWebGetArg(A,B,C)               (( void (*)(const char*,char*,size_t) )        jt[126])(A,B,C)
#define jWebRestart(A)                  (( void (*)(uint32_t) )                        jt[127])(A)
#define jWebServer_hasArg(A)            (( bool (*)(const char *) )                    jt[128])(A)
#define jWebServer_on(A,B,C)          (( void (*)(const char *, void (*)(void),uint8_t) ) jt[129])(A,B,C)
#define jatoi(A)                        (( int (*)(const char *) )                     jt[130])(A)
#define jstrcpy_P(A,B)                  (( char *(*)(char *, const char *) )           jt[131])(A,B)
#define SetTasmotaGlobal(A,B)           (( void (*)(uint32_t,uint32_t) )               jt[132])(A,B)
#define fixsfti(A)                      (( int32_t (*)(float) )                        jt[133])(A)
#define gtgtbl                          (( void *(*)(void) )                           jt[134])
#define asettings                       ( SETTINGS **)                                 jt[135]
#define getspi(A)                       (( void *(*)(uint8_t))                          jt[136])(A)
#define jspi_begin(A,B,C,D,E)           (( void (*)(void *,uint8_t,int8_t,int8_t,int8_t )) jt[137])(A,B,C,D,E)
#define jspi_write(A,B)                 (( void (*)(void *,uint8_t))                     jt[138])(A,B)
#define jspi_writeBytes(A,B,C)          (( void (*)(void *,uint8_t*,uint32_t))           jt[139])(A,B,C)
#define jspi_Transaction(A,B,C)         (( void (*)(void *,uint8_t,uint32_t))            jt[140])(A,B,C)
#define jspi_transfer(A,B)              (( uint8_t (*)(void *,uint8_t))                  jt[141])(A,B)
#define jfile_open(A,B)                 (( void * (*)(const char*,char))                 jt[142])(A,B)
#define jfile_close(A)                  (( void (*)(void*))                              jt[143])(A)
#define jfile_seek(A,B,C)               (( int32_t (*)(void*,uint32_t,uint32_t))         jt[144])(A,B,C)
#define jfile_read(A,B,C)               (( int32_t (*)(void*,void*,uint32_t))           jt[145])(A,B,C)
#define jfile_write(A,B,C)              (( int32_t (*)(void*,void*,uint32_t))           jt[146])(A,B,C)
#define jCharToFloat(A)                 (( float (*)(char*))                            jt[147])(A)
#define jAddLogData(A,B)                (( void  (*)(uint32_t,const char*))             jt[148])(A,B)
#define jfexists(A)                     (( int32_t (*)(const char*))                    jt[149])(A)
#define jstrncmp_P(A,B,C)               (( int (*)(const char *, const char *, size_t)) jt[150])(A,B,C)
#define jspecial_malloc(A)              (( void * (*)(uint32_t))                        jt[151])(A)
#define jResponseCmndChar(A)            (( void (*)(char *))                            jt[152])(A)
#define jstrtol(A,B,C)                  (( int32_t (*)(char *,char **,size_t ))             jt[153])(A,B,C)
#define judp(A,B,C,D)                   (( uint32_t (*)(void *,uint32_t,uint32_t,uint32_t )) jt[154])(A,B,C,D)
#define ji2s(A,B)                       (( uint32_t (*)(uint32_t,uint32_t))             jt[155])(A,B)
#define jtaskc(A)                       (( uint32_t (*)(TASKPARS* ))                    jt[156])(A)
#define jtaskd(A)                       (( uint32_t (*)(uint32_t))                       jt[157])(A)
#define jPlugin_Get_SensorNames(A,B)    (( char *(*)(char *,uint32_t))                  jt[158])(A,B)
#define GetScriptSection_P(A)           (( char *(*)(const char *))                     jt[159])(A)
#define jfile_size(A)                   (( uint32_t (*)(void*))                         jt[160])(A)
#define jfile_getpos(A)                 (( uint32_t (*)(void*))                         jt[161])(A)
#define jOsWatchLoop()                  (( void (*)(void))                              jt[162])
#define double_dispatch(A,B,C)          (( double (*)(uint32_t,double,double))          jt[163])(A,B,C)
#define d2i64(A)                        (( double (*)(int64_t))                         jt[164])(A)
#define i642d(A)                        (( int64_t (*)(double))                         jt[165])(A)
#define jMqttPublishSensor()            (( void (*)(void))                              jt[166])
#define jParseParameters(A,B)           (( uint32_t (*)(uint32_t,uint32_t *))           jt[167])(A,B)
#define jtmod__modsi3(A,B)              (( int32_t (*)(int32_t,int32_t) )               jt[168])(A,B)
#define jtmod__ashldi3(A,B)             (( int64_t (*)(int64_t,int32_t) )               jt[169])(A,B)
#define jtmod__lshrdi3(A,B)             (( uint64_t (*)(uint64_t,uint32_t) )            jt[170])(A,B)
#define jtmod_wifi(A,B,C,D,E)           (( uint32_t (*)(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t) ) jt[171])(A,B,C,D,E)
#define iUrlEncode(A)                   (( char * (*)(char *))                          jt[171])(A)
#define jstrncat_P(A,B,C)               (( void *(*)(char *,const char*,uint32_t))      jt[172])(A,B,C)
#define jpgm_read_byte(A)               (( uint8_t(*)(const void *))                    jt[173])(A)
#define jpgm_read_word(A)               (( uint16_t(*)(const void *))                   jt[174])(A)
#define jspdispatch(A,B,C,D)            (( uint32_t (*)(uint32_t,uint32_t,uint32_t,uint32_t) ) jt[175])(A,B,C,D)
#define jdtostrfd(A,B,C)                (( char *(*)(double,unsigned char,char*))       jt[176])(A,B,C)
#define jReplace_Cmd_Vars(A,B,C,D)      (( void (*)(uint32_t,uint32_t,uint32_t,uint32_t) ) jt[177])(A,B,C,D)
#define PinUsed(A)                      (( bool(*)(uint32_t))                            jt[178])(A)
#define jatoll(A)                       (( int64_t(*)(char*))                            jt[179])(A)
#define double_cdispatch(A,B,C)         (( int32_t (*)(uint32_t,double,double))          jt[180])(A,B,C)
//#define __floatdidf(A)                  (( double(*)(int64_t))                           jt[181])(A)
//#define __floatundidf(A)                (( double(*)(uint64_t))                          jt[182])(A)
#define p__floatsidf(A)                  (( double(*)(int32_t))                           jt[183])(A)
#define p__floatunsidf(A)                (( double(*)(uint32_t))                          jt[184])(A)
#define p__fixdfdi(A)                    (( int32_t(*)(double))                           jt[185])(A)
#define p__fixunsdfsi(A)                 (( uint32_t(*)(double))                          jt[186])(A)
#define p__extendsfdf2(A)                (( double(*)(float))                             jt[187])(A)
#define random(A)                       (( uint32_t(*)(uint32_t))                        jt[188])(A)
#define realloc(A,B)                    (( void *(*)(void*,size_t))                      jt[189])(A,B)
#define p__floattidf(A)                  (( double(*)(int64_t))                           jt[190])(A)
#define p__floatuntidf(A)                (( double(*)(uint64_t))                          jt[191])(A)

#define jbr_aes_small_ctr_init(A,B,C)    (( void (*)(br_aes_small_ctr_keys*,const void*,size_t)) jt[192])(A,B,C)
#define jbr_gcm_init(A,B,C)              (( void (*)(br_gcm_context*,const br_block_ctr_class**,br_ghash)) jt[193])(A,B,C)
#define jbr_gcm_reset(A,B,C)             (( void (*)(br_gcm_context*,const void*,size_t)) jt[194])(A,B,C)
#define jbr_gcm_aad_inject(A,B,C)        (( void (*)(br_gcm_context*,const void*,size_t)) jt[195])(A,B,C)
#define jbr_gcm_flip(A)                  (( void (*)(br_gcm_context*))                    jt[196])(A)
#define jbr_gcm_run(A,B,C,D)             (( void (*)(br_gcm_context*,int,void*,size_t))   jt[197])(A,B,C,D)
#define jbr_gcm_check_tag_trunc(A,B,C)   (( int32_t (*)(br_gcm_context*,const void*,size_t)) jt[198])(A,B,C)

#define vsnprintf_P(A,B,C,...)           (( int32_t (*)(char *,size_t,const char *,...))   jt[199])(A,B,C,##__VA_ARGS__)
#define makeTime(A)                      (( time_t (*)(const tmElements_t))               jt[200])(A)
#define jGetPins                         (( uint32_t(*)(void))                            jt[201])
#define jwc(A,B,C)                       (( uint32_t(*)(uint32_t,uint32_t,uint32_t))      jt[202])(A,B,C)
#define jdrwjpeg(A,B,C,D,E)              (( uint32_t(*)(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t)) jt[203])(A,B,C,D,E)
#define jshine(A,B,C,D)                  (( uint32_t(*)(uint32_t,uint32_t,uint32_t,uint32_t)) jt[204])(A,B,C,D)
#define jsinf(A)                         (( float (*)(float))                            jt[205])(A)
#define jcosf(A)                         (( float (*)(float))                            jt[206])(A)
#define jlogf(A)                         (( float (*)(float))                            jt[207])(A)
#define jsqrtf(A)                        (( float (*)(float))                            jt[208])(A)
#define jexpf(A)                         (( float (*)(float))                            jt[215])(A)
// Append-only dual-bus I2C (jt[216..218]). NEW slots — jI2cWrite8
// (jt[45], 3-arg) is left byte-identical so existing plugins are
// unaffected. Native2dual-scaffolded drivers opt into these locally.
#define jI2cWrite8Bus(A,R,V,B)           (( bool (*)(uint32_t,uint32_t,uint32_t,uint32_t)) jt[216])(A,R,V,B)
#define jI2cWrite0(A,R,B)                (( bool (*)(uint32_t,uint32_t,uint32_t))          jt[217])(A,R,B)
#define jI2cReadBuffer0(A,BUF,L,B)       (( bool (*)(uint32_t,uint8_t*,uint32_t,uint32_t)) jt[218])(A,BUF,L,B)

// jt[219] — ONE selector-dispatched slot for many helpers (keeps the
// frozen JMPTBL minimal: a future helper adds a `case` in
// tmod_ext_call, NEVER a new jt slot). 0..218 byte-identical.
#define jEXT(SEL,A,B,C)   (( int32_t (*)(uint32_t,uint32_t,uint32_t,uint32_t)) jt[219])((SEL),(A),(B),(C))
#define jI2cRead8Bus(A,R,B)     ((uint8_t)  jEXT(0,(A),(R),(B)))
#define jI2cRead16LE(A,R,B)     ((uint16_t) jEXT(1,(A),(R),(B)))
#define jCalcTempHumToDew(T,H)  (__extension__({ float _t=(T),_h=(H); uint32_t _a,_b; \
                                  __builtin_memcpy(&_a,&_t,4); __builtin_memcpy(&_b,&_h,4); \
                                  int32_t _r=jEXT(2,_a,_b,0); float _f; \
                                  __builtin_memcpy(&_f,&_r,4); _f; }))
#define jTempUnit()             ((char)     jEXT(3,0,0,0))
#define jI2cRead24(A,R,B)       ((int32_t)  jEXT(4,(A),(R),(B)))
#define jI2cReadS16_LE(A,R,B)   ((int16_t)  jEXT(5,(A),(R),(B)))
#define jPressureUnit()         ((const char*)(intptr_t) jEXT(6,0,0,0))
#define jConvertPressure(P)              (__extension__({ float _p=(P); uint32_t _a; \
                                  __builtin_memcpy(&_a,&_p,4); int32_t _r=jEXT(7,_a,0,0); \
                                  float _f; __builtin_memcpy(&_f,&_r,4); _f; }))
#define jConvertPressureForSeaLevel(P)   (__extension__({ float _p=(P); uint32_t _a; \
                                  __builtin_memcpy(&_a,&_p,4); int32_t _r=jEXT(8,_a,0,0); \
                                  float _f; __builtin_memcpy(&_f,&_r,4); _f; }))
// Additive global aliases (no jt change): a scaffolded driver calls
// these natural names. I2cRead8/24/16LE/S16_LE stay FILE-LOCAL remaps
// (the frozen 2-arg `#define I2cRead8 jI2cRead8` must remain for
// existing plugins); these two have no frozen form to protect.
#define jResponseCmndIdxNumber(V)        ((void)   jEXT(9,(uint32_t)(int)(V),0,0))
#define jSqrtInt(N)                      ((uint32_t)jEXT(10,(uint32_t)(N),0,0))
#define jGetPin(L)                       ((uint32_t)jEXT(11,(uint32_t)(L),0,0))
#define jDigitalWrite(G,I,S)             ((void)   jEXT(12,(uint32_t)(G),(uint32_t)(I),(uint32_t)(S)))
#define CalcTempHumToDew jCalcTempHumToDew
#define TempUnit jTempUnit
#define ConvertPressure jConvertPressure
#define ConvertPressureForSeaLevel jConvertPressureForSeaLevel
#define ResponseCmndIdxNumber jResponseCmndIdxNumber
#define SqrtInt jSqrtInt
#define GetPin jGetPin
#define DigitalWrite jDigitalWrite

// PicoTTS engine API — exposed by firmware lib/libesp32_div/pico/.
// Plugin code calls picotts_init/picotts_add/etc. naturally; macros at
// the bottom of this file remap to the j-prefixed versions which go
// through the jumptable.
#define jpicotts_init(P,C,A)             (( bool (*)(unsigned, void(*)(int16_t*,unsigned), int) ) jt[209])(P,C,A)
#define jpicotts_add(T,L)                (( void (*)(const char*, unsigned) )                    jt[210])(T,L)
#define jpicotts_shutdown()              (( void (*)(void) )                                     jt[211])()
#define jpicotts_set_idle_notify(C)      (( void (*)(void(*)(void)) )                            jt[212])(C)
#define jpicotts_set_error_notify(C)     (( void (*)(void(*)(void)) )                            jt[213])(C)
#define jpicotts_set_resources(T,S)      (( void (*)(const void*, const void*) )                 jt[214])(T,S)

// float negation via sign-bit XOR (no intrinsic needed, 1u<<31 is PIC-safe)
#define fneg(a) ({ float _fneg_tmp = (a); *(uint32_t*)&_fneg_tmp ^= (1u << 31); _fneg_tmp; })

// inline abs to avoid external function call from PIC code
#undef abs
#define abs(x) ({ typeof(x) _abs_tmp = (x); _abs_tmp < 0 ? -_abs_tmp : _abs_tmp; })

// Arduino macros
#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
#define bitSet(value, bit) ((value) |= (1UL << (bit)))
#define bitClear(value, bit) ((value) &= ~(1UL << (bit)))
#ifndef bitWrite
#define bitWrite(value, bit, bitvalue) (bitvalue ? bitSet(value, bit) : bitClear(value, bit))
#endif
#define fldsiz(name, field) (sizeof(((name *)0)->field))

#ifdef ESP8266
#define PLUGIN_CODE_TEXT
#endif

// essential defines -----------------------------------------------------------------------
// linker sections
#ifdef PLUGIN_CODE_TEXT
#define SECTION_DESC ".text.mod_desc"
#define SECTION_STRING ".text.mod_string"
#define SECTION_PART ".text.mod_part"
#define SECTION_END ".text.mod_end"
#else
#define SECTION_DESC ".plugin.mod_desc"
#define SECTION_STRING ".plugin.mod_string"
#define SECTION_PART ".plugin.mod_part"
#define SECTION_END ".plugin.mod_end"
#endif
//KEEP (*(SORT(.text.mod.*)))

#ifndef MODULE_HEADER
#define MODULE_HEADER module_header
#endif

#define MODULE_FUNCTION_EXECUTE mod_func_execute
#define END_OF_MODULE end_of_module

//#define MODULE_DESC __attribute__((section(SECTION_DESC))) extern const FLASH_MODULE
#ifdef ESP32
//#define MODULE_PART __attribute__( (section(SECTION_PART),aligned(4))) 
#define MODULE_PART __attribute__( (section(SECTION_PART),aligned(4))) __attribute__( (optimize("no-stack-protector")) ) 
#define MODULE_END __attribute__((section(SECTION_END),aligned(4))) static void  END_OF_MODULE(void) {__asm__ __volatile__(".align 4\n.word 0x4AFCAA55");}
#else
#define MODULE_PART __attribute__((section(SECTION_PART)))
#define MODULE_END __attribute__((section(SECTION_END))) static void  END_OF_MODULE(void) {__asm__ __volatile__(".word 0x4AFCAA55");}
#endif


#ifdef ESP32
#ifdef __riscv
#undef MODULE_PSTART

#define MODULE_PSTART
 //   _Pragma("GCC options push") \
 //    _Pragma("GCC optimize ("-O1")")

#undef MODULE_PEND
#define MODULE_PEND
  //  _Pragma("GCC options pop") \

#else
#undef MODULE_PSTART
#define MODULE_PSTART
#undef MODULE_PEND
#define MODULE_PEND
#endif
#endif

#ifdef ESP8266
#undef MODULE_PSTART
#define MODULE_PSTART
#undef MODULE_PEND
#define MODULE_PEND
#endif


// #pragma GCC optimize ("-fno-stack-protector")

//redefine_extname oldname newname
//#pragma redefine_extname myroutine __fixed_myroutine

//#define GET_MTABLE static uint32_t  GetmTbl(void) {
//  return 0x12345678;
//}

//#pragma GCC push_options
//#pragma GCC optimize ("-Og")
//#pragma GCC optimize ("-O3")
//#pragma GCC optimize ("-fno-stack-protector")
//#pragma GCC pop_options

/*
xtensa-esp32-elf-objdump -d ./.pio/build/tasmota32-4M/firmware.elf >dissasm.txt
riscv32-esp-elf-objdump -d ./.pio/build/tasmota32c3-4M/firmware.elf >dissasm.txt

riscv_save - riscv_restore

gcc -Q --help=optimizers
gcc -Q --help=target

riscv32-esp-elf-gcc -Q -O0 --help=optimizers >opts_0
riscv32-esp-elf-gcc -Q -O3 --help=optimizers >opts_3
diff opts_0 opts_3 | grep enabled

*/

typedef struct {
  uint32_t      nanos;
  uint8_t       second;
  uint8_t       minute;
  uint8_t       hour;
  uint8_t       day_of_week;               // sunday is day 1
  uint8_t       day_of_month;
  uint8_t       month;
  char          name_of_month[4];
  uint16_t      day_of_year;
  uint16_t      year;
  uint32_t      days;
  uint32_t      valid;
}TIME_T;

typedef struct {
  uint16_t      valid;                     // 290  (RTC memory offset 100)
  uint8_t       oswatch_blocked_loop;      // 292
  uint8_t       ota_loader;                // 293
  uint32_t      ex_energy_kWhtoday;        // 294
  uint32_t      ex_energy_kWhtotal;        // 298
  volatile uint32_t pulse_counter[MAX_COUNTERS];  // 29C - See #9521 why volatile
  power_t       power;                     // 2AC
  EnergyUsage   energy_usage;              // 2B0
  uint32_t      nextwakeup;                // 2C8
  uint32_t      baudrate;                  // 2CC
  uint32_t      ultradeepsleep;            // 2D0
  uint16_t      deepsleep_slip;            // 2D4
  uint8_t       improv_state;              // 2D6

  uint8_t       free_2d7[1];               // 2D7

  int32_t       energy_kWhtoday_ph[3];     // 2D8
  int32_t       energy_kWhtotal_ph[3];     // 2E4
  int32_t       energy_kWhexport_ph[3];    // 2F0
  uint32_t      utc_time;                  // 2FC
} TRtcSettings;


typedef struct { 
  uint16_t *tele_period;
  uint32_t *global_update;
  float *temperature_celsius;
  float *humidity;
  uint32_t *uptime;
  power_t *rel_inverted;
  uint8_t *devices_present;
  uint8_t *spi_enabled;
  uint8_t *soft_spi_enabled;
  TIME_T *RtcTime;
  StateBitfield *global_state;
  uint16_t *gpio_pin;
  TRtcSettings *rtc;
  bool *i2c_enabled;          // append-only (TGTAB idx 13); 0..12 unchanged
} GTBL;

#define STGLOB  GTBL *tgbl = (GTBL*) gtgtbl(); TRtcSettings  *RtcSettings = tgbl->rtc;


#define SCRIPT_EOL 10

#define TasmotaGlobal  *tgbl

//#define PROGMEM  __attribute__((section(".irom.text")))
#undef PROGMEM
#ifdef PLUGIN_CODE_TEXT
#define PROGMEM  __attribute__((section(".text.mod_string"),aligned(4)))
#else
#define PROGMEM  __attribute__((section(".plugin.mod_string"),aligned(4)))
#endif


//#define PSTR(s) (__extension__({static const char __c[] PROGMEM = (s); &__c[0];}))
#undef PSTR
#define PSTR(s) (__extension__({static const char __c[] PROGMEM = (s); &__c[EXEC_OFFSET];}))
#define GSTR(LABEL) (const char *)LABEL+EXEC_OFFSET
#define GU8(LABEL) (const uint8_t *)LABEL+EXEC_OFFSET
#define GFLT(LABEL) (float *) ((char *)LABEL+EXEC_OFFSET)
#define PU8(s) (__extension__({static const uint8_t __c[] PROGMEM = (s); &__c[EXEC_OFFSET];}))
#define GVOID(LABEL) (void (*)(void*))((uint8_t*)LABEL+EXEC_OFFSET)
#define GUI16p(LABEL) (const unsigned short *) ((uint8_t *)LABEL+EXEC_OFFSET)
#define GUI32p(LABEL) (const uint32_t *) ((uint8_t *)LABEL+EXEC_OFFSET)

// all floating point constants must be in progmem and named FP_CONST
//#define FLTC(INDEX) *(float *) ((char *)&FP_CONST[INDEX]+EXEC_OFFSET)
// FLTC: use volatile uint32_t read (forces l32i) then memcpy to float to avoid lsi from IROM on ESP32-S3
#define FLTC(INDEX) ({ volatile uint32_t _tmp = ((const volatile uint32_t*)((char*)FP_CONST + EXEC_OFFSET))[INDEX]; float _ftmp; __builtin_memcpy(&_ftmp, (void*)&_tmp, 4); _ftmp; })

//#define VTABLE(A) void (*const A[])(MODULES_TABLE*) PROGMEM
#define VTABLE(A) void (*const A[])(void) PROGMEM

#define GVT(LABEL) ( void (**)(MODULES_TABLE*) ) ((char *)LABEL+EXEC_OFFSET)

#define FSTRING(A) const char A[] PROGMEM 
#define FU8ARRAY(A) const uint8_t A[] PROGMEM 
#define GU8A(LABEL) (const uint8_t *)LABEL+EXEC_OFFSET

#define PARRAY(A,...) (__extension__({static const unsigned char __c[] PROGMEM = {A,...}; &__c[EXEC_OFFSET];}))

#define GFB(A)  pgm_read_byte(&A[EXEC_OFFSET])

extern "C" { MODULES_TABLE *gettbl(void); };

#undef pgm_read_byte
#undef pgm_read_word
#define pgm_read_byte jpgm_read_byte
#define pgm_read_word jpgm_read_word


#ifdef ESP32
extern const FLASH_MODULE module_header;
MODULE_PART MODULES_TABLE *gettbl();
#endif

extern "C" {
 extern void (* const MODULE_JUMPTABLE[])(void);
}
extern MODULES_TABLE modules[];

// counter 7 config 2  R/W = 0x3FF5705C

#ifdef ESP32
// esp32
#ifdef __riscv
#undef GET_MTBL
//#define GET_MTBL volatile MODULES_TABLE *mt = (MODULES_TABLE*)*(uint32_t*)GLOB_MOD_REG;
#define GET_MTBL volatile MODULES_TABLE *mt = gettbl()
#else
#undef GET_MTBL
#define GET_MTBL volatile MODULES_TABLE *mt = gettbl()
#endif

#undef GET_JT
#define GET_JT void (* const *jt)() = mt->jt

#else
// esp8266
#undef GET_MTBL
#define GET_MTBL MODULES_TABLE *mt = gettbl()
#undef GET_JT
#define GET_JT void (* const *jt)() = mt->jt
#endif

#define SETREGS GET_MTBL; MODULE_MEMORY *mem = (MODULE_MEMORY*)mt->mod_memory;GET_JT;FLASH_MODULE *mp = (FLASH_MODULE*)mt->mod_addr;SETTINGS *jsettings = *asettings;
#define ALLOCMEM GET_MTBL; GET_JT; mt->mem_size = sizeof(MODULE_MEMORY);mt->mem_size += mt->mem_size % 4;mt->mod_memory = jcalloc(mt->mem_size / 4, 4);if (!mt->mod_memory) {return -1;};MODULE_MEMORY *mem = (MODULE_MEMORY*)mt->mod_memory;SETTINGS *jsettings = *asettings;FLASH_MODULE *mp = (FLASH_MODULE*)mt->mod_addr;
#define RETMEM if (mt->mem_size) {jfree(mt->mod_memory);mt->mem_size = 0;}
#define MODULE_DESCRIPTOR(NAME,TYPE,REV,GPIO1,PIN1,GPIO2,PIN2,GPIO3,PIN3,GPIO4,PIN4)  __attribute__((section(SECTION_DESC))) extern const FLASH_MODULE MODULE_HEADER = {MODULE_SYNC,CURR_ARCH,(TYPE),(REV),(NAME),mod_func_execute,END_OF_MODULE,0,0,(uint32_t)&modules,(uint32_t)&MODULE_JUMPTABLE,(uint32_t)&module_header,mod_func_execute,{GPIO1,PIN1,GPIO2,PIN2,GPIO3,PIN3,GPIO4,PIN4}};
#define MODULE_DESCRIPTOR6(NAME,TYPE,REV,GPIO1,PIN1,GPIO2,PIN2,GPIO3,PIN3,GPIO4,PIN4,GPIO5,PIN5,GPIO6,PIN6)  __attribute__((section(SECTION_DESC))) extern const FLASH_MODULE MODULE_HEADER = {MODULE_SYNC,CURR_ARCH|(MAX_MOD_STORES<<24),(TYPE),(REV),(NAME),mod_func_execute,END_OF_MODULE,0,0,(uint32_t)&modules,(uint32_t)&MODULE_JUMPTABLE,(uint32_t)&module_header,mod_func_execute,{GPIO1,PIN1,GPIO2,PIN2,GPIO3,PIN3,GPIO4,PIN4,GPIO5,PIN5,GPIO6,PIN6}};
#define MODULE_DESCRIPTOR8(NAME,TYPE,REV,GPIO1,PIN1,GPIO2,PIN2,GPIO3,PIN3,GPIO4,PIN4,GPIO5,PIN5,GPIO6,PIN6,GPIO7,PIN7,GPIO8,PIN8)  __attribute__((section(SECTION_DESC))) extern const FLASH_MODULE MODULE_HEADER = {MODULE_SYNC,CURR_ARCH|(MAX_MOD_STORES<<24),(TYPE),(REV),(NAME),mod_func_execute,END_OF_MODULE,0,0,(uint32_t)&modules,(uint32_t)&MODULE_JUMPTABLE,(uint32_t)&module_header,mod_func_execute,{GPIO1,PIN1,GPIO2,PIN2,GPIO3,PIN3,GPIO4,PIN4,GPIO5,PIN5,GPIO6,PIN6,GPIO7,PIN7,GPIO8,PIN8}};
#define MODULE_DESCRIPTOR10(NAME,TYPE,REV,GPIO1,PIN1,GPIO2,PIN2,GPIO3,PIN3,GPIO4,PIN4,GPIO5,PIN5,GPIO6,PIN6,GPIO7,PIN7,GPIO8,PIN8,GPIO9,PIN9,GPIO10,PIN10)  __attribute__((section(SECTION_DESC))) extern const FLASH_MODULE MODULE_HEADER = {MODULE_SYNC,CURR_ARCH|(MAX_MOD_STORES<<24),(TYPE),(REV),(NAME),mod_func_execute,END_OF_MODULE,0,0,(uint32_t)&modules,(uint32_t)&MODULE_JUMPTABLE,(uint32_t)&module_header,mod_func_execute,{GPIO1,PIN1,GPIO2,PIN2,GPIO3,PIN3,GPIO4,PIN4,GPIO5,PIN5,GPIO6,PIN6,GPIO7,PIN7,GPIO8,PIN8,GPIO9,PIN9,GPIO10,PIN10}};


#define MOD_FUNC(A, ...) A(MODULES_TABLE *mt, ##__VA_ARGS__)
//#define MOD_FUNC(A, ...) A(##__VA_ARGS__)

#define SETMINREGS GET_MTBL; GET_JT;
#define SETMEMREGS GET_MTBL; GET_JT;MODULE_MEMORY *mem = (MODULE_MEMORY*)mt->mod_memory;

#define CALL_MOD_FUNC(A, ...) A(mt, ##__VA_ARGS__)

#define MOD_RESULT int32_t

#define STRBUFFER

/*
typedef struct {
  void (*xbeginTransmission)(uint8_t);
  uint8_t (*xendTransmission)(bool); 
  uint8_t (*xread)(); 
  void (*xwrite)(uint8_t);
  void (*xrequestFrom)(uint8_t,uint8_t);
}  xTwoWire;

#define INITWIRE(A) A->xbeginTransmission = ( void (*)(uint8_t) ) jt[12];A->xendTransmission = ( uint8_t (*)(bool) ) jt[14];A->xread = ( uint8_t (*)() ) jt[16];A->xwrite = ( void (*)(uint8_t) ) jt[13];A->xrequestFrom = ( void (*)(uint8_t,uint8_t) ) jt[15];
*/


#define initialized mt->flags.initialized
#define TasmotaSerial  void
//#define TwoWire xTwoWire

//#define   beginTransmission(ADDR) jbeginTransmission(mem->xWire, ADDR)
#define   I2cWrite(CMD) jwrite(mem->xWire, CMD)
#define   I2cWriten(BUF,LEN) jwriten(mem->xWire,BUF,LEN)

//#define   endTransmission(BUS) jendTransmission(mem->xWire, BUS)
//#define   requestFrom(ADDR,NUM)  jrequestFrom(mem->xWire, ADDR, NUM)
#define   I2cRead() jread(mem->xWire)
#define   I2cRead8 jI2cRead8
#define   I2cRead16 jI2cRead16
#define   I2cWrite16 jI2cWrite16
#define   I2cWrite8 jI2cWrite8
#define   delay jdelay
#define   I2cAvailable() javailable(mem->xWire)
#define   ConvertHumidity jConvertHumidity
#define   GetTextIndexed jGetTextIndexed
#define   I2cSetActiveFound jI2cSetActiveFound
#define   I2cActive jI2cActive
#define   AddLogMissed jAddLogMissed
#define   TempHumDewShow jTempHumDewShow
#define   GetTasmotaGlobal JGetTasmotaGlobal
#define   ConvertTemp jConvertTemp
#define   strlcpy jstrlcpy
#undef   snprintf_P
#define   snprintf_P jsnprintf_P
#define   TempHumDewShow jTempHumDewShow
#define   IndexSeparator jIndexSeparator
#define   ResponseAppend_P jResponseAppend_P
#define   Response_P jResponse_P
#define   ResponseJsonEndEnd jResponseJsonEndEnd
#define   ResponseJsonEnd jResponseJsonEnd
#define   XdrvRulesProcess jXdrvRulesProcess
#define   WSContentSend_PD jWSContentSend_PD
#define   WSContentSend_P jWSContentSend_P


#define GetPins jGetPins

//#define   WSContentSend_P(A,...) {char *xyz=jcopyStr(A); jWSContentSend_P(xyz,__VA_ARGS__); free(xyz);}

#define   I2cValidRead16 jI2cValidRead16
#define   I2cResetActive jI2cResetActive
#define   ftostrfd jftostrfd
#define   fscale jfscale
#define   I2cSetDevice jI2cSetDevice
#define   Pin jPin
#define   NewTS jnewTS
#define   writeTS jwriteTS
#define   flushTS jflushTS
#define   beginTS jbeginTS
#define   XdrvMailbox (jXdrvMailbox)
#define   GetCommandCode jGetCommandCode
#define   strlen jstrlen
#undef strncasecmp_P
#define   strncasecmp_P jstrncasecmp_P
#define   toupper jtoupper
#define   iscale jiscale
#define   deleteTS jdeleteTS
#define   readTS jreadTS
#define   readbTS jread1TS
#define   availTS javailTS
#define   IndexSeparator jIndexSeparator
#define   AddLog jAddLog
#define   MqttPublishTeleSensor jMqttPublishTeleSensor
#define   strtoul jstrtoul
#define   AddLogBuffer jAddLogBuffer
#define   ResponseTime_P jResponseTime_P
#define   ClaimSerial jClaimSerial
#define   hardwareSerial jhardwareSerial
#define   millis jmillis
#undef    sprintf_P
#define   sprintf_P jsprintf_P
#define   AddLogT jAddlogT
#define   tmod__divsi3 jtmod__divsi3
#define   tmod__udivsi3 jtmod__udivsi3
#define   tmod__floatsisf jtmod__floatsisf
#define   tmod__floatunsisf jtmod__floatunsisf
#define   FastPrecisePowf  jFastPrecisePowf
#define   isnan jisnan
#define   isinf jisinf
#define   copyStr jcopyStr
#define   tmod__mulsf3  jfmul
#define   tmod__divsf3  jfdiv
#define   tmod__addsf3  jfadd
#define   tmod__subsf3  jfdiff

#define   GetTasmotaGlobalf JGetTasmotaGlobalf
#define   tmod__muldi3 jtmod__muldi3
#define   tmod__fixunssfsi jtmod__fixunssfsi
#define   tmod__umodsi3 jtmod__umodsi3
#define   __umodsi3 jtmod__umodsi3
#define   twi_readFrom jtwi_readFrom
#define   DecodeCommand(A,B) jDecodeCommand(A,B,mt)
#define   ResponseCmndDone jResponseCmndDone
#define   bwriteTS jbwriteTS
#define   memcmp jmemcmp
#undef memcmp_P
#define   memcmp_P jmemcmp
#define   ToHex_P(A,B,C,D) jToHex_P(A,B,C,D,'\0') 
#define   memset jmemset
#define   memmove jmemmove
#define   memmove_P jmemmove
#define   ResponseCmndNumber jResponseCmndNumber
#define   ResponseCmndFloat jResponseCmndFloat
#define   ResponseAppendTHD jResponseAppendTHD
#define   WSContentSend_THD jWSContentSend_THD
#undef memcpy_P
#define   memcpy_P jmemmove
#define   memcpy jmemmove
#define   strncpy jstrncpy
#undef strncpy_P
#define   strncpy_P jstrncpy
#define   isprint jisprint
#define   setClockStretchLimit(VAL) jsetClockStretchLimit(mem->xWire, VAL)
#define   writen(BUF,LEN) jwriten(mem->xWire,BUF,LEN)
#define free jfree
#define modff jmodff
#define fl_const jfl_const
#define WSContentSend_Temp jWSContentSend_Temp
#define delayMicroseconds jdelayMicroseconds
#define digitalRead jdigitalRead
#define digitalWrite jdigitalWrite
#define pinMode jpinMode
#define sprintf jsprintf_P
#define Settings jsettings
#define strchr jstrchr
#define trimm jtrimm
#define vTaskEnterCritical jvTaskEnterCritical
#define vTaskExitCritical jvTaskExitCritical
#define directRead jdirectRead
#define directWriteLow jdirectWriteLow
#define directWriteHigh jdirectWriteHigh
#define directModeInput jdirectModeInput
#define directModeOutput jdirectModeOutput
#define CalcTempHumToAbsHum jCalcTempHumToAbsHum
#define tofloat jtofloat
#define strtol jstrtol
#define atoll jatoll

typedef union {
	uint8_t bytes[4];  // IPv4 address
	uint32_t dword;
} IP_ADDRESS;


#define Draw_JPEG(A,B,C,D,E) jdrwjpeg((uint32_t)A,B,C,D,E)

#ifdef USE_PSHINE

#define Shine_initialise p_shine_initialise
#define Shine_set_config_mpeg_defaults p_shine_set_config_mpeg_defaults
#define Shine_check_config p_shine_check_config
#define Shine_samples_per_pass p_shine_samples_per_pass
#define Shine_encode_buffer_interleaved p_shine_encode_buffer_interleaved
#define Shine_flush p_shine_flush
#define Shine_close p_shine_close

// PIC table access macros for embedded shine encoder
#define GTAB_I32(LABEL) ((const int32_t*)((char*)(LABEL) + EXEC_OFFSET))
#define GTAB_U8(LABEL) ((const uint8_t*)((char*)(LABEL) + EXEC_OFFSET))
#define GTAB_U16(LABEL) ((const uint16_t*)((char*)(LABEL) + EXEC_OFFSET))
#define GHUFF(idx) ((const struct p_huffcodetab*)((char*)p_shine_huffman_table + EXEC_OFFSET))[idx]
/* All huffman tables are now int32_t - safe 32-bit aligned access on all platforms */
#define GHUFF_HLEN(h, p) (GTAB_I32((h).hlen)[p])
#define GHUFF_TABLE(h, p) (GTAB_I32((h).table)[p])
#else
#define Shine_initialise(A) jshine(0,(uint32_t)A,0,0)
#define Shine_set_config_mpeg_defaults(A) jshine(1,(uint32_t)A,0,0)
#define Shine_check_config(A,B) jshine(2,A,B,0)
#define Shine_samples_per_pass(A) jshine(3,(uint32_t)A,0,0)
#define Shine_encode_buffer_interleaved(A,B,C) jshine(4,(uint32_t)A,(uint32_t)B,(uint32_t)C)
#define Shine_flush(A,B) jshine(5,(uint32_t)A,(uint32_t)B,0)
#define Shine_close(A) jshine(6,(uint32_t)A,0,0)
#endif

#define webcam_init(A) jwc(0,A,0)
#define webcam_GetFrame(A) jwc(1,A,0)
#define webcam_SetOptions(A,B) jwc(2,A,B)
#define webcam_GetWidth() jwc(3,0,0)
#define webcam_GetHeight() jwc(4,0,0)
#define webcam_Stream(A) jwc(5,A,0)
#define webcam_PicStore(A,B) jwc(6,A,(uint32_t)B)

#define new_udp()  (void*)judp(0,0,0,0)
#define udp_begin(udp,port)  judp(udp,2,port,0)
#define udp_beginPacket(udp,ip,port) judp(udp,7,(uint32_t)ip,port)
#define udp_write(udp,ptr,len) judp(udp, 8, (uint32_t)ptr, len)
#define udp_endPacket(udp)  judp(udp,9,0,0)
#define udp_stop(udp)  judp(udp,1,0,0)
#define udp_del(udp) judp(udp,99,0,0)
#define udp_flush(udp) judp(udp,6,0,0)
#define udp_parsePacket(udp) judp(udp,3,0,0)
#define udp_read(udp,buff,len) judp(udp,5,(uint32_t)buff,len)
#define udp_remoteIP(udp) judp(udp,10,0,0)
#define udp_available(udp) judp(udp,4,0,0)


#define iseq jiseq
#define fixunssfsi tmod__fixunssfsi
#define ltsf2 jltsf2
#define gtsf2 jgtsf2
#define floatunsisf jtmod__floatunsisf
#define udivsi3 jtmod__udivsi3
#define HttpCheckPriviledgedAccess jHttpCheckPriviledgedAccess
#define WSContentStart_P jWSContentStart_P
#define WebServer jWebServer
#define WSContentSendStyle jWSContentSendStyle
#define WSContentSpaceButton jWSContentSpaceButton
#define WSContentStop jWSContentStop
#define WebGetArg jWebGetArg
#define WebRestart jWebRestart
#define WebServer_hasArg jWebServer_hasArg
#define WebServer_on(A,B) jWebServer_on(A,(void (*)(void)) ((uint32_t)B + EXEC_OFFSET),HTTP_ANY)
#define atoi jatoi
#undef strcpy_P
#define strcpy_P jstrcpy_P
#define GETSPI(A) mem->spi = getspi(A);
#define dtostrfd jdtostrfd

#define spi_begin() jspi_begin(mem->spi,0,-1,-1,-1)
#define spi_end() if (mem->spi) jspi_begin(mem->spi,1,-1,-1,-1)
#define spiBeginTransaction() jspi_Transaction(mem->spi,0,this->spibaud)
#define spiEndTransaction() jspi_Transaction(mem->spi,1,0)
#define spiTransfer(A) jspi_transfer(mem->spi,A)

#define fopen(A,B)  jfile_open(A,B)
#define fclose(A) jfile_close(A)
#define fseek(A,B,C) jfile_seek(A,B,C)
//#define fread(A,B,C)   jfile_read(A,B,C) 
//#define fwrite(A,B,C) jfile_write(A,B,C)

#define fread(A,B,C,D)   jfile_read(D,A,B*C) 
#define fwrite(A,B,C,D) jfile_write(D,A,B*C)
#define fsize(A) jfile_size(A)
#define fpos(A) jfile_getpos(A)
#define OsWatchLoop jOsWatchLoop

#define i2s_begin_t(A) (void*)ji2s(0,(uint32_t)A)
#define i2s_begin_r(A) (void*)ji2s(0x100,(uint32_t)A)

#define i2s_end_t(A) ji2s(1,(uint32_t)A)
#define i2s_end_r(A) ji2s(0x101,(uint32_t)A)

#define i2s_set_rate_t(A) ji2s(2,(uint32_t)A)
#define i2s_set_rate_r(A) ji2s(0x102,(uint32_t)A)

#define i2s_write_sample_t(A) ji2s(5,(uint32_t)A)
#define i2s_write_sample_r(A) ji2s(0x105,(uint32_t)A)

#define i2s_write_samples_t(A) ji2s(3,(uint32_t)A)
#define i2s_write_samples_r(A) ji2s(0x103,(uint32_t)A)

#define i2s_read_samples_t(A) ji2s(4,(uint32_t)A)
#define i2s_read_samples_r(A) ji2s(0x104,(uint32_t)A)

#define i2s_enable_tx(A) ji2s(6,(uint32_t)A)
#define i2s_disable_tx(A) ji2s(7,(uint32_t)A)

#define i2s_enable_rx(A) ji2s(0x106,(uint32_t)A)
#define i2s_disable_rx(A) ji2s(0x107,(uint32_t)A)

#define i2s_channel_register_event_callback_t(A) ji2s(8,(uint32_t)A)
#define i2s_channel_register_event_callback_r(A) ji2s(0x108,(uint32_t)A)


#define New_WiFiClient() (void*)jtmod_wifi(0,0,0,0,0)
#define client_connect(A,B,C) (int32_t)jtmod_wifi(1,(uint32_t)A,(uint32_t)B,C,0)
#define client_connected(A) (int32_t)jtmod_wifi(2,(uint32_t)A,0,0,0)
#define client_available(A) (int32_t)jtmod_wifi(3,(uint32_t)A,0,0,0)
#define client_read(A) (uint8_t)jtmod_wifi(4,(uint32_t)A,0,0,0)
#define client_write(A,B,C) (uint32_t)jtmod_wifi(8,(uint32_t)A,(uint32_t)B,C,0)
#define client_readn(A,B,C) (int32_t)jtmod_wifi(5,(uint32_t)A,(uint32_t)B,C,0)
#define client_stop(A) jtmod_wifi(6,(uint32_t)A,0,0,0)
#define client_delete(A) jtmod_wifi(7,(uint32_t)A,0,0,0)
#define client_peek(A) jtmod_wifi(9,(uint32_t)A,0,0,0)
#define client_flush(A) jtmod_wifi(100,(uint32_t)A,0,0,0)
#define client_setTimeout(A,B) jtmod_wifi(101,(uint32_t)A,B,0,0)
#define client_print(A,B) jtmod_wifi(102,(uint32_t)A,(uint32_t)B,0,0)
// ESP32-only — set TCP SO_LINGER on a WiFiClient. on=1, time=0 → next stop()/
// delete sends RST instead of FIN, freeing peers (e.g. SMA Tripower 10.0SE)
// that allow only one TCP session and would otherwise reject reconnects after
// a script reload. ESP8266's WiFiClient has no setSocketOption() — this is a
// no-op there; callers should fall back to plain client_stop().
#define client_setLinger(A,ON,T) jtmod_wifi(103,(uint32_t)A,ON,T,0)


#define ipa_fromstring(A,B) jtmod_wifi(70,(uint32_t)A,(uint32_t)B,0,0)
#define ipa_tostring(A,B) jtmod_wifi(71,(uint32_t)A,(uint32_t)B,0,0)
#define attachInterruptArg(A,B,C,D) jtmod_wifi(72,(uint32_t)A,(uint32_t)B,(uint32_t)C,D)
#define detachInterrupt(A) jtmod_wifi(73,A,0,0,0)


#define NewWebServer(A) (void*)jtmod_wifi(80,A,0,0,0)
#define WebServerOn(A,B,C) jtmod_wifi(81,(uint32_t)A,(uint32_t)B,(uint32_t)C,0)
#define WebServerBegin(A) jtmod_wifi(82,(uint32_t)A,0,0,0)
#define WebServerStop(A) jtmod_wifi(83,(uint32_t)A,0,0,0)
#define NewWebServerGetClient(A) (void*)jtmod_wifi(84,(uint32_t)A,0,0,0)
#define WebServerHandleClient(A) jtmod_wifi(85,(uint32_t)A,0,0,0)
#define WebServerDelete(A) jtmod_wifi(86,(uint32_t)A,0,0,0)

#define WebServerClientPrint(A,B)   jtmod_wifi(87,(uint32_t)A,(uint32_t)B,0,0)
#define WebServerClientWrite(A,B,C) jtmod_wifi(88,(uint32_t)A,(uint32_t)B,C,0)
#define WebServerClientStop(A)      jtmod_wifi(89,(uint32_t)A,0,0,0)
#define WebServerClientConnected(A) jtmod_wifi(90,(uint32_t)A,0,0,0)
#define WebServerClientFlush(A)     jtmod_wifi(91,(uint32_t)A,0,0,0)
#define WebServerClientSetTimeout(A,B) jtmod_wifi(92,(uint32_t)A,B,0,0)

#define New_WiFiClientSecure() (void*)jtmod_wifi(10,0,0,0,0)
#define sclient_connect(A,B,C) (int32_t)jtmod_wifi(11,(uint32_t)A,(uint32_t)B,C,0)
#define sclient_connected(A) (int32_t)jtmod_wifi(12,(uint32_t)A,0,0,0)
#define sclient_available(A) (int32_t)jtmod_wifi(13,(uint32_t)A,0,0,0)
#define sclient_read(A) (uint8_t)jtmod_wifi(14,(uint32_t)A,0,0,0)
#define sclient_readn(A,B,C) (int32_t)jtmod_wifi(15,(uint32_t)A,(uint32_t)B,C,0)
#define sclient_stop(A) jtmod_wifi(16,(uint32_t)A,0,0,0)
#define sclient_delete(A) jtmod_wifi(17,(uint32_t)A,0,0,0)
#define sclient_setInsecure(A) jtmod_wifi(18,(uint32_t)A,0,0,0)
#define sclient_setTimeout(A,B) jtmod_wifi(19,(uint32_t)A,B,0,0)


#define New_HTTP() (void*)jtmod_wifi(30,0,0,0,0)
#define http_end(A) jtmod_wifi(31,(uint32_t)A,0,0,0)
#define http_delete(A) jtmod_wifi(32,(uint32_t)A,0,0,0)
#define http_begin(A,B,C) (bool)jtmod_wifi(33,(uint32_t)A,(uint32_t)B,(uint32_t)C,0)
#define http_setReuse(A,B) jtmod_wifi(34,(uint32_t)A,B,0,0)
#define http_GET(A) (int32_t)jtmod_wifi(35,(uint32_t)A,0,0,0)
#define http_getSize(A) (int32_t)jtmod_wifi(36,(uint32_t)A,0,0,0)
#define http_connected(A) (bool)jtmod_wifi(37,(uint32_t)A,0,0,0)
#define http_getStreamPtr(A) (void*)jtmod_wifi(38,(uint32_t)A,0,0,0)
#define http_addHeader(A,B,C) jtmod_wifi(39,(uint32_t)A,(uint32_t)B,(uint32_t)C,0)
#define http_collectHeaders(A,B,C) jtmod_wifi(40,(uint32_t)A,(uint32_t)B,(uint32_t)C,(uint32_t)mt)
#define http_header(A,B)(char*)jtmod_wifi(41,(uint32_t)A,(uint32_t)B,0,0)
#define http_hasHeader(A,B) jtmod_wifi(42,(uint32_t)A,(uint32_t)B,0,0)
#define http_setFollowRedirects(A,B) jtmod_wifi(43,(uint32_t)A,(uint32_t)B,0,0)
#define http_begin1(A,B,C) (bool)jtmod_wifi(44,(uint32_t)A,(uint32_t)B,(uint32_t)C,0)

// HTTPClientLight (BearSSL https-capable) — self-contained: begin(url) does its
// own TLS; getStreamPtr() returns the body WiFiClient* (read via plain client_*).
#define New_HTTPLight()       (void*)jtmod_wifi(45,0,0,0,0)
#define httpl_begin(A,B)      (bool)jtmod_wifi(46,(uint32_t)A,(uint32_t)B,0,0)
#define httpl_GET(A)          (int32_t)jtmod_wifi(47,(uint32_t)A,0,0,0)
#define httpl_getStreamPtr(A) (void*)jtmod_wifi(48,(uint32_t)A,0,0,0)
#define httpl_connected(A)    (bool)jtmod_wifi(49,(uint32_t)A,0,0,0)
#define httpl_end(A)          jtmod_wifi(54,(uint32_t)A,0,0,0)
#define httpl_delete(A)       jtmod_wifi(55,(uint32_t)A,0,0,0)


// tasmota serial
#define New_TSerial(A) (void*)jspdispatch(0,0,(uint32_t)A,0)
#define TSerial_End(A) jspdispatch(1,(uint32_t)A,0,0)
#define Del_TSerial(A) jspdispatch(2,(uint32_t)A,0,0)
#define TSerial_Begin(A,B,C) jspdispatch(3,(uint32_t)A,B,C)
#define TSerial_Available(A) jspdispatch(4,(uint32_t)A,0,0)
#define TSerial_Peek(A) jspdispatch(5,(uint32_t)A,0,0)
#define TSerial_Read(A) jspdispatch(6,(uint32_t)A,0,0)
#define TSerial_Write(A,B,C) jspdispatch(7,(uint32_t)A,(uint32_t)B,C)
#define TSerial_Flush(A) jspdispatch(8,(uint32_t)A,0,0)
#define TSerial_Hardwareserial(A) jspdispatch(9,(uint32_t)A,0,0)


#define New_E32Serial(A) (void*)jspdispatch(20,0,A,0)
#define E32Serial_End(A) jspdispatch(21,(uint32_t)A,0,0)
#define Del_E32Serial(A) jspdispatch(22,(uint32_t)A,0,0)
#define E32Serial_Begin(A,B) jspdispatch(23,(uint32_t)A,(uint32_t)B,0)
#define E32Serial_Available(A) jspdispatch(24,(uint32_t)A,0,0)
#define E32Serial_Peek(A) jspdispatch(25,(uint32_t)A,0,0)
#define E32Serial_Read(A) jspdispatch(26,(uint32_t)A,0,0)
#define E32Serial_Write(A,B,C) jspdispatch(27,(uint32_t)A,(uint32_t)B,C)
#define E32Serial_Flush(A) jspdispatch(28,(uint32_t)A,0,0)

#define E32Serial_SetBaudrate(A,B) jspdispatch(29,(uint32_t)A,B,0)
#define E32Serial_RxBufferSize(A,B) jspdispatch(30,(uint32_t)A,B,0)

#define E32_SOC_UART_HP_NUM jspdispatch(50,0,0,0)

#define jgpio_pullup_dis(A) jspdispatch(51,A,0,0)


// canbus
#define ptwai_driver_install(A,B,C) jspdispatch(70,(uint32_t)A,(uint32_t)B,(uint32_t)C)
#define ptwai_driver_uninstall() jspdispatch(71,0,0,0)
#define ptwai_start() jspdispatch(72,0,0,0)
#define ptwai_stop() jspdispatch(73,0,0,0)
#define ptwai_reconfigure_alerts(A,B) jspdispatch(74,(uint32_t)A,(uint32_t)B,0)
#define ptwai_get_status_info(A) jspdispatch(75,(uint32_t)A,0,0)
#define ptwai_receive(A,B) jspdispatch(76,(uint32_t)A,(uint32_t)B,0)
#define ptwai_transmit(A,B) jspdispatch(77,(uint32_t)A,(uint32_t)B,0)
#define ptwai_read_alerts(A,B) jspdispatch(78,(uint32_t)A,(uint32_t)B,0)
#define ptwai_clear_receive_queue() jspdispatch(79,0,0,0)

#define ValidPin(A) jspdispatch(100,A,A,0)

#define Replace_Cmd_Vars(A,B,C,D) jReplace_Cmd_Vars((uint32_t)A,B,(uint32_t)C,D)

// float and double conversions
#define float_i32 jtmod__floatsisf
#define float_ui32 jtmod__floatunsisf
#define float_ui64 jtofloat
#define i32_float
#define ui32_float jtmod__fixunssfsi

#define double_i64 p__floattidf
#define double_ui64 p__floatuntidf
#define double_i32 p__floatsidf
#define double_ui32 p__floatunsidf
#define i32_double p__fixdfdi
#define ui32_double p__fixunsdfsi
#define i64_double i642d
#define double_float p__extendsfdf2

// float and double math
#define fmul jfmul
#define fadd jfadd
#define fdiv jfdiv
#define fdiff jfdiff

#ifdef USE_PSHINE
#define sinf jsinf
#define cosf jcosf
#define logf jlogf
#define sqrtf jsqrtf
#define expf jexpf
#endif

#define dadd(A,B) double_dispatch(0,A,B)
#define ddiff(A,B) double_dispatch(1,A,B)
#define dmul(A,B) double_dispatch(2,A,B)
#define ddiv(A,B) double_dispatch(3,A,B)


#define GetHostbyName(A,B) jtmod_wifi(60,0,(uint32_t)A,(uint32_t)B,0)


#define xTaskCreatePinnedToCore(A)   jtaskc(A)
#define vTaskDelete(A)   jtaskd(A)

#undef strncat_P
#define strncat_P jstrncat_P

#ifndef USE_BP_DOUBLES
#define __adddf3(A,B) double_dispatch(0,A,B)
#define __subdf3(A,B) double_dispatch(1,A,B)
#define __muldf3(A,B) double_dispatch(2,A,B)
#define __divdf3(A,B) double_dispatch(3,A,B)

#define __ltdf2(A,B) double_cdispatch(0,A,B)
#define __nedf2(A,B) double_cdispatch(1,A,B)
#endif

// in64 to double
//#define __floatdidf(A) d2i64(A)
// uint64 to double
//#define __floatundidf(A) d2i64(A)
// in32 to double
//#define __floatsidf(A) d2i64(A)
// uint32 to double
//#define __floatunsidf(A) d2i64(A)

// double to int32
//#define __fixdfdi(A) d2i64(A)
// double to uint32
//#define __fixunsdfsi(A) d2i64(A)

// float to double
//#define __extendsfdf2(A) d2i64(A)


#define dadd(A,B) double_dispatch(0,A,B)
#define dsub(A,B) double_dispatch(1,A,B)
#define dmul(A,B) double_dispatch(2,A,B)
#define ddiv(A,B) double_dispatch(3,A,B)

#define MqttPublishSensor jMqttPublishSensor
#define ParseParameters jParseParameters

#ifdef USE_SOFTWIRE 
#define MAX_I2C_Busses 1
#define I2C_SETWIRE(A) New_SWI2C(mp->ms[0].value, mp->ms[1].value);
#define I2C_beginTransmission SWI2C_beginTransmission
#define I2C_endTransmission SWI2C_endTransmission
#define I2C_requestFrom(A,B) SWI2C_requestFrom(A,B,true)
#define I2C_write SWI2C_Write
#define I2C_WriteN SWI2C_Writen
#define I2C_write8 I2cWrite8

#define I2C_read SWI2C_Read
#define I2C_Read8 I2cRead8
#define I2C_available I2cAvailable
#define I2C_ResetActive(A,B) SWI2C_delete()
#define I2C_SetDevice(A,B) SWI2C_SetDevice(A)
#define I2C_SetActiveFound(A,B,C) SWI2C_SetActiveFound(A,B)
#define TWIp SWI2C_VARS
#define xWire swv
#define I2C_ValidRead16 SWI2C_ValidRead16
#define I2C_Read16 SWI2C_Read16
#define I2C_Write16 SWI2C_Write16
#define I2C_readFrom twi_readFrom
#else
#define MAX_I2C_Busses 2
#define I2C_SETWIRE SETWIRE
#define I2C_beginTransmission(ADDR) jbeginTransmission(mem->xWire, ADDR)
#define I2C_endTransmission(BUS) jendTransmission(mem->xWire, BUS)
#define I2C_requestFrom(ADDR,NUM) jrequestFrom(mem->xWire, ADDR, NUM)
#define I2C_write I2cWrite
#define I2C_WriteN I2cWriten
#define I2C_write8 I2cWrite8


#define I2C_read I2cRead
#define I2C_Read8 I2cRead8
#define I2C_available I2cAvailable
#define I2C_ResetActive I2cResetActive
#define I2C_SetDevice I2cSetDevice
#define I2C_SetActiveFound I2cSetActiveFound
#define TWIp TwoWire
#define I2C_ValidRead16 I2cValidRead16
#define I2C_Read16 I2cRead16
#define I2C_Write16 I2cWrite16
#define I2C_readFrom twi_readFrom

#endif





#define CharToFloat jCharToFloat
#define AddLogData jAddLogData
#define calloc jcalloc
#define malloc(A) jcalloc(A,1)
#define fexists jfexists
#undef strncmp_P
#define strncmp_P jstrncmp_P
#define special_malloc jspecial_malloc
#define ResponseCmndChar jResponseCmndChar
#define Plugin_Get_SensorNames jPlugin_Get_SensorNames

// PicoTTS — alias the natural names so plugin code uses
// `picotts_init(...)` etc. and they expand through the jumptable.
#define picotts_init             jpicotts_init
#define picotts_add              jpicotts_add
#define picotts_shutdown         jpicotts_shutdown
#define picotts_set_idle_notify  jpicotts_set_idle_notify
#define picotts_set_error_notify jpicotts_set_error_notify
#define picotts_set_resources    jpicotts_set_resources

#define __divsi3 jtmod__divsi3
#define __udivsi3 jtmod__udivsi3
#define __modsi3 jtmod__modsi3
#define __muldi3 jtmod__muldi3
#define __ashldi3 jtmod__ashldi3
#define __lshrdi3 jtmod__lshrdi3

//size_t fread ( void * ptr, size_t size, size_t count, FILE * stream );



#define File_p void

#define yield() delay(0)

#define this mem


#define FPC(A,B) jfl_const(A,B)

// floating point constants must be defined here
#ifdef __riscv
#define FPC_n999 jfl_const(-999,1)
#define FPC_0x01 jfl_const(1,100)
#define FPC_0x02 jfl_const(2,100)
#define FPC_273x15 jfl_const(27315,100)
#define FPC_0x00097656 jfl_const(97656,100000000)
#define FPC_0 jfl_const(0,0)
#else
#define FPC_n999 -999
#define FPC_0x01 0.01
#define FPC_0x02 0.02
#define FPC_273x15 273.15
#define FPC_0x00097656 0.00097656
#define FPC_0 jfl_const(0,0)

#endif

// floating point zero is always global symbol on esp32 
//FPC_0


// tensilica immediate is only -2048 to 2047
// all others must be coded with ICONST

#ifdef ESP8266
#define ICONST(A) A
#else
#ifdef __riscv
#define ICONST(A) A
#else
//#define ICONST(A) fixunssfsi(A)
#define ICONST(A) fixsfti(A)
#endif
#endif

#define SETWIRE(A) if (A==0) {mem->xWire = jWire;} else {mem->xWire = jWire1;} 

#define xSETWIRE(A) if (A==0) {xWire = jWire;} else {xWire = jWire1;} 

#ifdef __riscv
#define PUSH_OPTIONS _Pragma("GCC push_options")\
_Pragma("GCC optimize (\"-Og\")")
#define PULL_OPTIONS _Pragma("GCC pop_options")
#else
#ifdef ESP32
#define PUSH_OPTIONS _Pragma("GCC push_options")\
_Pragma("GCC optimize (\"-Og\")")
#define PULL_OPTIONS _Pragma("GCC pop_options")
#else
#define PUSH_OPTIONS
#define PULL_OPTIONS
#endif
#endif


/*
#define PUSH_OPTIONS \
#ifdef __riscv \
#pragma GCC push_options \
#pragma GCC optimize ("-Og") \
#endif
*/



/*
#if 0
#define RENAME_LIBRARY_SET ".set"
#define RENAME_LIBRARY(GCC_NAME, AEABI_NAME)		\
  __asm__ (".globl\t__" #AEABI_NAME "\n"		\
	   RENAME_LIBRARY_SET "\t__" #AEABI_NAME 	\
	     ", __" #GCC_NAME "\n");

#else
#define RENAME_LIBRARY(GCC_NAME, AEABI_NAME)			\
  __asm__ (".globl\t__c6xabi_" #AEABI_NAME "\n"		\
	   ".set\t__c6xabi_" #AEABI_NAME			\
	   ", __gnu_" #GCC_NAME "\n");
#endif

*/

/*
#define RENAME_LIBRARY(GCC_NAME, AEABI_NAME)			\
  __asm__ (".global\t__" #AEABI_NAME "\n"		\
	   ".set\t__" #AEABI_NAME			\
	   ", __" #GCC_NAME "\n");
*/

/*
#define RENAME_LIBRARY(OLD,NEW)						\
__asm__ (".global\t__" #NEW "\n"						\
	 "\t.set __" #NEW ",__" #OLD "\n"					\
	 "\t.type\t__" #NEW ",@function\n");



__asm__ (
" .section .text.mod_part\n"
" .align 4\n"
" .global	xmurks\n"
" .type   xmurks,@function\n"
" xmurks:\n"
" ret.n"
);



//#define DECLARE_LIBRARY_RENAMES 

RENAME_LIBRARY (addsf3,murks)

float __murks(float a,float b) {
  return a+b;
}
*/





/*
Next: Routines for decimal floating point emulation, Previous: Routines for integer arithmetic, Up: The GCC low-level runtime library   [Contents][Index]

4.2 Routines for floating point emulation

The software floating point library is used on machines which do not have hardware support for floating point. It is also used whenever -msoft-float is used to disable generation of floating point instructions. (Not all targets support this switch.)

For compatibility with other compilers, the floating point emulation routines can be renamed with the DECLARE_LIBRARY_RENAMES macro (see Implicit Calls to Library Routines). In this section, the default names are used.

Presently the library does not support XFmode, which is used for long double on some architectures.

Arithmetic functions
Conversion functions
Comparison functions
Other floating-point functions
4.2.1 Arithmetic functions

Runtime Function: float __addsf3 (float a, float b)
Runtime Function: double __adddf3 (double a, double b)
Runtime Function: long double __addtf3 (long double a, long double b)
Runtime Function: long double __addxf3 (long double a, long double b)
These functions return the sum of a and b.

Runtime Function: float __subsf3 (float a, float b)
Runtime Function: double __subdf3 (double a, double b)
Runtime Function: long double __subtf3 (long double a, long double b)
Runtime Function: long double __subxf3 (long double a, long double b)
These functions return the difference between b and a; that is, a - b.

Runtime Function: float __mulsf3 (float a, float b)
Runtime Function: double __muldf3 (double a, double b)
Runtime Function: long double __multf3 (long double a, long double b)
Runtime Function: long double __mulxf3 (long double a, long double b)
These functions return the product of a and b.

Runtime Function: float __divsf3 (float a, float b)
Runtime Function: double __divdf3 (double a, double b)
Runtime Function: long double __divtf3 (long double a, long double b)
Runtime Function: long double __divxf3 (long double a, long double b)
These functions return the quotient of a and b; that is, a / b.

Runtime Function: float __negsf2 (float a)
Runtime Function: double __negdf2 (double a)
Runtime Function: long double __negtf2 (long double a)
Runtime Function: long double __negxf2 (long double a)
These functions return the negation of a. They simply flip the sign bit, so they can produce negative zero and negative NaN.

4.2.2 Conversion functions

Runtime Function: double __extendsfdf2 (float a)
Runtime Function: long double __extendsftf2 (float a)
Runtime Function: long double __extendsfxf2 (float a)
Runtime Function: long double __extenddftf2 (double a)
Runtime Function: long double __extenddfxf2 (double a)
These functions extend a to the wider mode of their return type.

Runtime Function: double __truncxfdf2 (long double a)
Runtime Function: double __trunctfdf2 (long double a)
Runtime Function: float __truncxfsf2 (long double a)
Runtime Function: float __trunctfsf2 (long double a)
Runtime Function: float __truncdfsf2 (double a)
These functions truncate a to the narrower mode of their return type, rounding toward zero.

Runtime Function: int __fixsfsi (float a)
Runtime Function: int __fixdfsi (double a)
Runtime Function: int __fixtfsi (long double a)
Runtime Function: int __fixxfsi (long double a)
These functions convert a to a signed integer, rounding toward zero.

Runtime Function: long __fixsfdi (float a)
Runtime Function: long __fixdfdi (double a)
Runtime Function: long __fixtfdi (long double a)
Runtime Function: long __fixxfdi (long double a)
These functions convert a to a signed long, rounding toward zero.

Runtime Function: long long __fixsfti (float a)
Runtime Function: long long __fixdfti (double a)
Runtime Function: long long __fixtfti (long double a)
Runtime Function: long long __fixxfti (long double a)
These functions convert a to a signed long long, rounding toward zero.

Runtime Function: unsigned int __fixunssfsi (float a)
Runtime Function: unsigned int __fixunsdfsi (double a)
Runtime Function: unsigned int __fixunstfsi (long double a)
Runtime Function: unsigned int __fixunsxfsi (long double a)
These functions convert a to an unsigned integer, rounding toward zero. Negative values all become zero.

Runtime Function: unsigned long __fixunssfdi (float a)
Runtime Function: unsigned long __fixunsdfdi (double a)
Runtime Function: unsigned long __fixunstfdi (long double a)
Runtime Function: unsigned long __fixunsxfdi (long double a)
These functions convert a to an unsigned long, rounding toward zero. Negative values all become zero.

Runtime Function: unsigned long long __fixunssfti (float a)
Runtime Function: unsigned long long __fixunsdfti (double a)
Runtime Function: unsigned long long __fixunstfti (long double a)
Runtime Function: unsigned long long __fixunsxfti (long double a)
These functions convert a to an unsigned long long, rounding toward zero. Negative values all become zero.

Runtime Function: float __floatsisf (int i)
Runtime Function: double __floatsidf (int i)
Runtime Function: long double __floatsitf (int i)
Runtime Function: long double __floatsixf (int i)
These functions convert i, a signed integer, to floating point.

Runtime Function: float __floatdisf (long i)
Runtime Function: double __floatdidf (long i)
Runtime Function: long double __floatditf (long i)
Runtime Function: long double __floatdixf (long i)
These functions convert i, a signed long, to floating point.

Runtime Function: float __floattisf (long long i)
Runtime Function: double __floattidf (long long i)
Runtime Function: long double __floattitf (long long i)
Runtime Function: long double __floattixf (long long i)
These functions convert i, a signed long long, to floating point.

Runtime Function: float __floatunsisf (unsigned int i)
Runtime Function: double __floatunsidf (unsigned int i)
Runtime Function: long double __floatunsitf (unsigned int i)
Runtime Function: long double __floatunsixf (unsigned int i)
These functions convert i, an unsigned integer, to floating point.

Runtime Function: float __floatundisf (unsigned long i)
Runtime Function: double __floatundidf (unsigned long i)
Runtime Function: long double __floatunditf (unsigned long i)
Runtime Function: long double __floatundixf (unsigned long i)
These functions convert i, an unsigned long, to floating point.

Runtime Function: float __floatuntisf (unsigned long long i)
Runtime Function: double __floatuntidf (unsigned long long i)
Runtime Function: long double __floatuntitf (unsigned long long i)
Runtime Function: long double __floatuntixf (unsigned long long i)
These functions convert i, an unsigned long long, to floating point.

Runtime Function: void __fixsfbitint (UBILtype *r, int32_t rprec, float a)
Runtime Function: void __fixdfbitint (UBILtype *r, int32_t rprec, double a)
Runtime Function: void __fixxfbitint (UBILtype *r, int32_t rprec, __float80 a)
Runtime Function: void __fixtfbitint (UBILtype *r, int32_t rprec, _Float128 a)
These functions convert a to bit-precise integer r, rounding toward zero. If rprec is positive, it converts to unsigned bit-precise integer and negative values all become zero, if rprec is negative, it converts to signed bit-precise integer.

Runtime Function: float __floatbitintsf (UBILtype *i, int32_t iprec)
Runtime Function: double __floatbitintdf (UBILtype *i, int32_t iprec)
Runtime Function: __float80 __floatbitintxf (UBILtype *i, int32_t iprec)
Runtime Function: _Float128 __floatbitinttf (UBILtype *i, int32_t iprec)
Runtime Function: _Float16 __floatbitinthf (UBILtype *i, int32_t iprec)
Runtime Function: __bf16 __floatbitintbf (UBILtype *i, int32_t iprec)
These functions convert bit-precise integer i to floating point. If iprec is positive, it is conversion from unsigned bit-precise integer, otherwise from signed bit-precise integer.

4.2.3 Comparison functions

There are two sets of basic comparison functions.

Runtime Function: int __cmpsf2 (float a, float b)
Runtime Function: int __cmpdf2 (double a, double b)
Runtime Function: int __cmptf2 (long double a, long double b)
These functions calculate a <=> b. That is, if a is less than b, they return −1; if a is greater than b, they return 1; and if a and b are equal they return 0. If either argument is NaN they return 1, but you should not rely on this; if NaN is a possibility, use one of the higher-level comparison functions.

Runtime Function: int __unordsf2 (float a, float b)
Runtime Function: int __unorddf2 (double a, double b)
Runtime Function: int __unordtf2 (long double a, long double b)
These functions return a nonzero value if either argument is NaN, otherwise 0.

There is also a complete group of higher level functions which correspond directly to comparison operators. They implement the ISO C semantics for floating-point comparisons, taking NaN into account. Pay careful attention to the return values defined for each set. Under the hood, all of these routines are implemented as

  if (__unordXf2 (a, b))
    return E;
  return __cmpXf2 (a, b);
where E is a constant chosen to give the proper behavior for NaN. Thus, the meaning of the return value is different for each set. Do not rely on this implementation; only the semantics documented below are guaranteed.

Runtime Function: int __eqsf2 (float a, float b)
Runtime Function: int __eqdf2 (double a, double b)
Runtime Function: int __eqtf2 (long double a, long double b)
These functions return zero if neither argument is NaN, and a and b are equal.

Runtime Function: int __nesf2 (float a, float b)
Runtime Function: int __nedf2 (double a, double b)
Runtime Function: int __netf2 (long double a, long double b)
These functions return a nonzero value if either argument is NaN, or if a and b are unequal.

Runtime Function: int __gesf2 (float a, float b)
Runtime Function: int __gedf2 (double a, double b)
Runtime Function: int __getf2 (long double a, long double b)
These functions return a value greater than or equal to zero if neither argument is NaN, and a is greater than or equal to b.

Runtime Function: int __ltsf2 (float a, float b)
Runtime Function: int __ltdf2 (double a, double b)
Runtime Function: int __lttf2 (long double a, long double b)
These functions return a value less than zero if neither argument is NaN, and a is strictly less than b.

Runtime Function: int __lesf2 (float a, float b)
Runtime Function: int __ledf2 (double a, double b)
Runtime Function: int __letf2 (long double a, long double b)
These functions return a value less than or equal to zero if neither argument is NaN, and a is less than or equal to b.

Runtime Function: int __gtsf2 (float a, float b)
Runtime Function: int __gtdf2 (double a, double b)
Runtime Function: int __gttf2 (long double a, long double b)
These functions return a value greater than zero if neither argument is NaN, and a is strictly greater than b.

4.2.4 Other floating-point functions

Runtime Function: float __powisf2 (float a, int b)
Runtime Function: double __powidf2 (double a, int b)
Runtime Function: long double __powitf2 (long double a, int b)
Runtime Function: long double __powixf2 (long double a, int b)
These functions convert raise a to the power b.

Runtime Function: complex float __mulsc3 (float a, float b, float c, float d)
Runtime Function: complex double __muldc3 (double a, double b, double c, double d)
Runtime Function: complex long double __multc3 (long double a, long double b, long double c, long double d)
Runtime Function: complex long double __mulxc3 (long double a, long double b, long double c, long double d)
These functions return the product of a + ib and c + id, following the rules of C99 Annex G.

Runtime Function: complex float __divsc3 (float a, float b, float c, float d)
Runtime Function: complex double __divdc3 (double a, double b, double c, double d)
Runtime Function: complex long double __divtc3 (long double a, long double b, long double c, long double d)
Runtime Function: complex long double __divxc3 (long double a, long double b, long double c, long double d)
These functions return the quotient of a + ib and c + id (i.e., (a + ib) / (c + id)), following the rules of C99 Annex G.

Next: Routines for decimal floating point emulation, Previous: Routines for integer arithmetic, Up: The GCC low-level runtime library   [Contents][Index]


SML math

__ltdf2
	.global	__fixdfdi  double to long
	.global	__floatdidf  long to double
	.global	__divdf3
	.global	__muldf3
	.global	__subdf3


	.global	__mulsf3
	.global	__muldi3
  
  .global	__udivsi3 uin32 / uin32
	.global	__umodsi3 uin32 % uin32

  .global	__floatsidf int to double

	.global	__floatunsidf
	.global	__floatundidf
	.global	__extendsfdf2
	.global	__floatsisf
	.global	__floatunsisf
	.global	__fixdfsi
	.global	__truncdfsf2
	.global	__fixunsdfsi
	__nedf2


*/
#if 0
typedef struct {
  uint8_t sda;
  uint8_t scl;
  uint8_t inputMode;
  uint8_t delay_us;
  uint16_t timeout_ms;
  uint8_t rxBuffer[16];
  uint8_t rxBufferSize;
  uint8_t rxBufferIndex;
  uint8_t rxBufferBytesRead;
  uint8_t txAddress;
  uint8_t txBuffer[16];
  uint8_t txBufferSize;
  uint8_t txBufferIndex;
  bool transmissionInProgress;
  bool allowClockStretch;
  uint32_t lastmillis;
} SWI2C_VARS;
#else


#define SWI2C_BUFFER_LENGTH 16
typedef struct {
  uint8_t rxBuffer[SWI2C_BUFFER_LENGTH];
  uint8_t rxBufferIndex;
  uint8_t rxBufferLength;
  uint8_t isTransmitting;
  uint8_t error;
  uint8_t sda;
  uint8_t scl;
} SWI2C_VARS;

#endif
