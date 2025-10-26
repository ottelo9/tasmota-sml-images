#ifndef _SML_SCRIPTS_H_
#define _SML_SCRIPTS_H_

//adapted from this source
//https://github.com/esplesekopf/Tasmota/blob/development/tasmota/sml_scripts.h

#define SCRIPT_SML_DOWNLOAD_URL "https://raw.githubusercontent.com/esplesekopf/Scripts/master/scripts/"
#define SCRIPT_SML_DOWNLOAD_URL_SMARTMETERS "https://raw.githubusercontent.com/esplesekopf/Scripts/master/smartmeters.json"

#define SCRIPT_ISKRA_MT_681_SML "" \
    ">D\\n" \
    ">B\\n" \
    "=>sensor53 r\\n" \
    ">M 1\\n" \
    "+1,14,s,0,9600,MT681\\n" \
    "1,77070100010800ff@1000,Total Consumed,KWh,Total_in,3\\n" \
    "1,77070100100700ff@1,Current Consumption,W,Power_cur,0\\n" \
    "1,77070100240700ff@1,Current Consumption P1,W,Power_p1,0\\n" \
    "1,77070100380700ff@1,Current Consumption P2,W,Power_p2,0\\n" \
    "1,770701004c0700ff@1,Current Consumption P3,W,Power_p3,0\\n" \
    "1,77070100020800ff@1000,Total Delivered,KWh,Total_out,3\\n" \
    "1,77070100000009ff@#,Service ID,,Meter_id,0|\\n" \
    "#\\n"

#define SCRIPT_HAGER_EHZ161_OBIS "" \
    ">D\\n" \
    ">B\\n" \
    "->sensor53 r\\n" \
    ">M 1\\n" \
    "+1,14,o,0,9600,OBIS\\n" \
    "1,1-0:1.8.1\\\\*255(@1,Total consumption,KWh,Total_in,4\\n" \
    "1,1-0:2.8.1\\\\*255(@1,Total Feed,KWh,Total_out,4\\n" \
    "1,=d 2 10 @1,Current consumption,W,Power_curr,0\\n" \
    "1,1-0:0.0.0\\\\*255(@#),Meter Nr,, Meter_number,0\\n" \
    "#\\n"

#define SCRIPT_SML_SELECT_OPTIONS "" \
    "<option value='sm_0'>--- Select SmartMeter ---</option>" \
    "<option value='sm_mt681'>Iskra MT 681 (SML)</option>" \
    "<option value='sm_ehz161'>Hager EHZ161 (OBIS)</option>"

#define SCRIPT_SML_SELECT_FUNCTION "" \
    "if(selSM.value=='sm_0'){ta.innerHTML=''}" \
    "else if(selSM.value=='sm_mt681'){ta.innerHTML='" SCRIPT_ISKRA_MT_681_SML "'}" \
    "else if(selSM.value=='sm_ehz161'){ta.innerHTML='" SCRIPT_HAGER_EHZ161_OBIS "'}" \
    "else{ta.innerHTML='';fetch('" SCRIPT_SML_DOWNLOAD_URL "'+selSM.value,{cache:'no-store'}).then(response=>response.text()).then(content=>{ta.innerHTML=content;});}"

#define SCRIPT_SML_SELECT "" \
    "<p><select id='idSelSM'>" SCRIPT_SML_SELECT_OPTIONS "</select></p>"

#define SCRIPT_SML_SELECT_HANDLER "" \
    "var selSM=eb('idSelSM');" \
    "selSM.onchange=function(){" SCRIPT_SML_SELECT_FUNCTION "};" \
    "fetch('" SCRIPT_SML_DOWNLOAD_URL_SMARTMETERS "',{cache:'no-store'}).then(response=>response.json()).then(data=>{" \
    "if(data && data.smartmeters && data.smartmeters.length){" \
    "while(selSM.options.length>1){selSM.options.remove(1);}" \
    "for(let n=0;n<data.smartmeters.length;n++){" \
    "let o=document.createElement('option');o.value=data.smartmeters[n].filename;o.text=data.smartmeters[n].label;selSM.options.add(o);" \
    "}}});"

#endif