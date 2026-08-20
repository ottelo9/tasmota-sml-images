/*
  xdrv_124_tinyc_repoide.h — "/tcrepo": compile a repo example straight onto
  this device, WITHOUT the IDE living on the device.

  Enabled with USE_TINYC_REPO_IDE. What the device stores is this ~4 KB page;
  the ~1 MB compiler is never on the flash — the browser pulls it from the repo
  (tinyc_ide.html.gz, gunzipped in the page) and runs it locally.

  Why it has to be served BY the device: an https page (e.g. the docs site on
  github.io) cannot talk to a plain-http LAN device — the browser blocks that as
  mixed content before any CORS header is read. Served from the device, upload
  and run are same-origin, and the only cross-origin traffic goes to GitHub,
  which answers with `access-control-allow-origin: *`.

  Flow: list examples (GitHub API) -> fetch source (raw) -> resolve #include
  from examples/ then examples/common/ -> compile in the browser -> POST the
  .tcb to /tc_upload?api=1 -> /tc_api?cmd=run. The compiler is whatever the
  repo currently ships, so device and toolchain can never drift apart.
*/

#ifndef _XDRV_124_TINYC_REPOIDE_H_
#define _XDRV_124_TINYC_REPOIDE_H_

#ifdef USE_TINYC_REPO_IDE

static const char TC_REPO_IDE_PAGE[] PROGMEM = R"XX(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>TinyC from repo</title>
<style>
body{margin:0;padding:16px;font:15px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;background:#252525;color:#eaeaea}
.w{max-width:640px;margin:0 auto}h3{margin:0 0 4px}p.s{margin:0 0 16px;color:#999;font-size:13px}
.r{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:10px 0}
select,button{font:inherit;padding:6px 10px;border-radius:5px;border:1px solid #555;background:#333;color:#eaeaea}
select{flex:1;min-width:180px}button{background:#1fa3ec;border-color:#1fa3ec;color:#fff;cursor:pointer}
button:disabled{opacity:.45;cursor:default}
#log{white-space:pre-wrap;font:12px/1.5 ui-monospace,Menlo,Consolas,monospace;background:#1f1f1f;color:#65c115;border-radius:5px;padding:10px;margin-top:12px;max-height:300px;overflow:auto}
a{color:#1fa3ec}
</style></head><body><div class="w">
<h3>TinyC from repo</h3>
<p class="s">Pick an example, pick a slot, Run. Compiled in this browser with the
compiler from the repo &mdash; nothing but this page lives on the device.</p>
<div class="r">
  <select id="ex"><option value="">loading list&hellip;</option></select>
  <select id="slot"><option>0</option><option>1</option><option>2</option><option>3</option><option>4</option><option>5</option></select>
  <button id="go" disabled>Run</button>
</div>
<div id="log">ready.</div>
<p class="s"><a href="/tc">TinyC console</a> &middot; <a href="/">main menu</a></p>
</div>
<script>
var OWNER='gemu2015',REPO='Sonoff-Tasmota',BR='universal';
var RAW='https://raw.githubusercontent.com/'+OWNER+'/'+REPO+'/'+BR+'/tasmota/tinyc';
var API='https://api.github.com/repos/'+OWNER+'/'+REPO+'/contents/tasmota/tinyc/examples?ref='+BR;
var $=function(i){return document.getElementById(i)},W=null,cache={};
function log(s){$('log').textContent+='\n'+s;$('log').scrollTop=1e9;}
function fail(s){log('FEHLER: '+s);$('go').disabled=false;}

// One file out of the repo, by path below tasmota/tinyc/. null = not there.
async function repoFile(path){
  if(cache[path]!==undefined)return cache[path];
  try{var r=await fetch(RAW+'/'+path);cache[path]=r.ok?await r.text():null;}
  catch(e){cache[path]=null;}
  return cache[path];
}

// The compiler, out of the shipped IDE bundle. It is gzipped in the repo, so it
// costs ~236 KB over the wire instead of ~1 MB. Its script block is one big
// chunk of UI code that throws without the IDE's DOM — but function
// declarations are hoisted, so compile() and resolveIncludesAsync() are there
// afterwards. Same trick build.html uses.
async function compiler(){
  if(W)return W;
  log('fetching compiler from repo…');
  var r=await fetch(RAW+'/tinyc_ide.html.gz');
  if(!r.ok)throw new Error('tinyc_ide.html.gz: HTTP '+r.status);
  var html;
  if(typeof DecompressionStream==='function'){
    var ds=new DecompressionStream('gzip');
    html=await new Response(r.body.pipeThrough(ds)).text();
  }else{throw new Error('browser cannot gunzip (needs Chrome 80+/Safari 16.4+)');}
  var m=html.match(/<script>([\s\S]*)<\/script>\s*<\/body>/);
  if(!m)throw new Error('no script block in the IDE bundle');
  var f=document.createElement('iframe');f.style.display='none';document.body.appendChild(f);
  try{f.contentWindow.eval(m[1]);}catch(e){}
  var w=f.contentWindow;
  if(typeof w.compile!=='function')throw new Error('compile() not found - IDE bundle too old?');
  W=w;log('compiler ready ('+(html.length/1024|0)+' KB).');return W;
}

async function list(){
  try{
    var r=await fetch(API,{headers:{'Accept':'application/vnd.github+json'}});
    if(!r.ok)throw new Error('GitHub API '+r.status);
    var f=await r.json(),skip=/^(test_|bug)|(_diag|selftest|smoke)/i,s=$('ex');
    var tc=f.filter(function(x){return x.type==='file'&&/\.tc$/.test(x.name)&&!skip.test(x.name);})
            .sort(function(a,b){return a.name.localeCompare(b.name);});
    s.innerHTML='';
    tc.forEach(function(x){var o=document.createElement('option');o.value=x.name;o.textContent=x.name.replace(/\.tc$/,'');s.appendChild(o);});
    log(tc.length+' examples in the repo.');$('go').disabled=false;
  }catch(e){
    $('ex').innerHTML='<option value="">list failed</option>';
    // 403 from the API is almost always the unauthenticated rate limit (60/h
    // per IP), which says nothing about the network - do not blame the WiFi.
    fail(e.message+(/403/.test(e.message)?' - GitHub rate limit (60 requests/h per IP), try again later'
                                         :' - no internet in this browser?'));
  }
}

$('go').onclick=async function(){
  var name=$('ex').value,slot=$('slot').value;
  if(!name)return;
  $('go').disabled=true;$('log').textContent='';
  try{
    var w=await compiler();
    log('loading '+name+'…');
    var src=await repoFile('examples/'+name);
    if(src===null)throw new Error(name+' not found in the repo');
    // #include blocks live in examples/common/ - same bare-filename rule the
    // compiler and the batch build use.
    var res=await w.resolveIncludesAsync(src,async function(n){
      var bare=n.replace(/^.*[\/\\]/,'');
      var t=await repoFile('examples/'+bare);
      if(t===null)t=await repoFile('examples/common/'+bare);
      if(t===null)throw new Error('#include "'+bare+'" not in the repo');
      log('  #include '+bare);
      return t;
    });
    var out=w.compile(res,{defines:[]});
    if(out.error)throw new Error(out.error);
    var bin=new Uint8Array(out.binary);
    log('compiled: '+bin.length+' bytes');
    var tcb=name.replace(/\.tc$/,'.tcb');
    var fd=new FormData();
    fd.append('file',new Blob([bin],{type:'application/octet-stream'}),tcb);
    var up=await fetch('/tc_upload?api=1&slot='+slot+'&fsz='+bin.length,{method:'POST',body:fd});
    if(!up.ok)throw new Error('upload: HTTP '+up.status);
    var uj=await up.json().catch(function(){return {};});
    log('uploaded '+(uj.size||bin.length)+' bytes to slot '+slot);
    var rr=await fetch('/tc_api?cmd=run&slot='+slot);
    var rj=await rr.json().catch(function(){return {};});
    if(rj.ok===false)throw new Error('run: '+(rj.error||'unknown'));
    log('running: '+((rj.file||uj.file||tcb).replace(/^\//,''))+'  ✓');
  }catch(e){fail(e.message);}
  $('go').disabled=false;
};
list();
</script></body></html>)XX";

// GET /tcrepo — hand out the page. Privileged like every other TinyC handler:
// it can overwrite a slot.
static void HandleTinyCRepoIde(void) {
  if (!HttpCheckPriviledgedAccess()) { return; }
  Webserver->send_P(200, PSTR("text/html"), TC_REPO_IDE_PAGE);
}

#endif  // USE_TINYC_REPO_IDE
#endif  // _XDRV_124_TINYC_REPOIDE_H_
