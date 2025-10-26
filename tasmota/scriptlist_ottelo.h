#ifndef _SCRIPTLIST_OTTELO_H_
#define _SCRIPTLIST_OTTELO_H_

// Powermeter List for Tasmota SML Script
// adapted from this source
// https://github.com/esplesekopf/Tasmota/blob/development/tasmota/sml_scripts.h
// and https://bitshake.de/skripte/#hersteller

#define SCRIPT_OTTELO_DOWNLOAD_URL "https://raw.githubusercontent.com/ottelo9/tasmota-sml-script/main/smartmeter/"
#define SCRIPT_OTTELO_DOWNLOAD_URL_SMARTMETERS "https://raw.githubusercontent.com/ottelo9/tasmota-sml-script/main/smartmeter/smartmeter.json"

#define SCRIPT_OTTELO_SELECT_OPTIONS "" \
    "<option value='sm_0'>--- Select SmartMeter ---</option>"

#define SCRIPT_OTTELO_SELECT_FUNCTION "" \
    "if(selSM.value=='sm_0'){ta.innerHTML=''}" \
    "else{ta.innerHTML='';fetch('" SCRIPT_OTTELO_DOWNLOAD_URL "'+selSM.value,{cache:'no-store'}).then(response=>response.text()).then(content=>{ta.innerHTML=content;});}"

#define SCRIPT_OTTELO_SELECT "" \
    "<p><select id='idSelSM'>" SCRIPT_OTTELO_SELECT_OPTIONS "</select></p>"

#define SCRIPT_OTTELO_SELECT_HANDLER "" \
    "var selSM=eb('idSelSM');" \
    "selSM.onchange=function(){" SCRIPT_OTTELO_SELECT_FUNCTION "};" \
    "fetch('" SCRIPT_OTTELO_DOWNLOAD_URL_SMARTMETERS "',{cache:'no-store'}).then(response=>response.json()).then(data=>{" \
    "if(data && data.smartmeter && data.smartmeter.length){" \
    "while(selSM.options.length>1){selSM.options.remove(1);}" \
    "for(let n=0;n<data.smartmeter.length;n++){" \
    "let o=document.createElement('option');o.value=data.smartmeter[n].filename;o.text=data.smartmeter[n].label;selSM.options.add(o);" \
    "}}});"

#endif