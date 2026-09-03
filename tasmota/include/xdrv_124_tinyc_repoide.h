/*
  xdrv_124_tinyc_repoide.h — "/tcrepo": run the FULL IDE from the repo,
  WITHOUT the IDE living on the device.

  Enabled with USE_TINYC_REPO_IDE. What the device stores is this ~3 KB page;
  the ~1 MB IDE is never on the flash — the browser pulls it from the repo
  (tinyc_ide.html.gz, ~236 KB over the wire, gunzipped here) and runs it in an
  iframe. Editing, compiling, uploading and running all happen in that IDE; the
  device only serves this loader and answers the IDE's requests.

  ⚠️ WHY IT HAS TO BE SERVED BY THE DEVICE. An https page (the docs site on
  github.io, say) cannot talk to a plain-http LAN device — the browser blocks
  that as mixed content before any CORS header is read. Served from the device,
  the iframe inherits this http origin, and the only cross-origin traffic goes
  to GitHub, which answers with `access-control-allow-origin: *`.

  ⚠️ THE ABI TAKES CARE OF ITSELF. The IDE asks the device for its
  TC_SYSCALL_ABI (/tc_api?cmd=status) and compiles DOWN to it — so a repo IDE
  newer than the firmware still produces bytecode this device can run. That is
  the whole reason the device IP is filled in below and a `change` event is
  dispatched: without it nothing triggers checkDeviceVersion(), and the IDE
  would compile at its own newest ABI.

  ⚠️ WHAT THIS DOES NOT DO: work offline. The on-device IDE
  (/tinyc_ide.html.gz) still exists and is still the right choice in the field
  or on an isolated network. This page makes it OPTIONAL, not forbidden — it
  buys back ~259 KB of filesystem where the network is there anyway.

  Was here before 2026-09-03: a small example picker that pulled ONE .tc from
  the repo, compiled it and ran it. It threw away the rest of the very bundle
  it had just downloaded. The full IDE brings its own example list (and the
  repo list too), so nothing is lost by rendering all of it.
*/

#ifndef _XDRV_124_TINYC_REPOIDE_H_
#define _XDRV_124_TINYC_REPOIDE_H_

#ifdef USE_TINYC_REPO_IDE

static const char TC_REPO_IDE_PAGE[] PROGMEM = R"XX(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>TinyC IDE from repo</title>
<style>
html,body{margin:0;height:100%;background:#252525;color:#eaeaea;
  font:14px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
#bar{display:flex;gap:10px;align-items:center;padding:6px 10px;background:#1f1f1f;
  border-bottom:1px solid #3a3a3a}
#bar b{font-weight:600}#st{color:#65c115;font:12px ui-monospace,Menlo,Consolas,monospace}
#st.e{color:#ff6b6b}
a{color:#1fa3ec}#ide{border:0;width:100%;height:calc(100% - 33px);display:none;background:#fff}
</style></head><body>
<div id="bar"><b>TinyC IDE</b><span id="st">loading from repo&hellip;</span>
<span style="flex:1"></span><a href="/tc">console</a><a href="/">menu</a></div>
<iframe id="ide"></iframe>
<script>
var RAW='https://raw.githubusercontent.com/gemu2015/Sonoff-Tasmota/universal/tasmota/tinyc';
var st=document.getElementById('st'),fr=document.getElementById('ide');
function say(s,bad){st.textContent=s;st.className=bad?'e':'';}

(async function(){
  try{
    if(typeof DecompressionStream!=='function')
      throw new Error('this browser cannot gunzip (needs Chrome 80+ / Safari 16.4+)');
    var r=await fetch(RAW+'/tinyc_ide.html.gz');
    if(!r.ok)throw new Error('tinyc_ide.html.gz: HTTP '+r.status);
    var html=await new Response(r.body.pipeThrough(new DecompressionStream('gzip'))).text();

    // Write the WHOLE document into the frame. document.write (rather than
    // srcdoc) keeps this page's origin AND its base URL, so the IDE's own
    // relative requests land on the device without any rewriting.
    var d=fr.contentDocument;
    d.open();d.write(html);d.close();

    // Hand the IDE the address it is already built to talk to, and fire the
    // `change` its listener waits for -- that is what makes it fetch
    // /tc_api?cmd=status and compile DOWN to this firmware's ABI.
    fr.onload=null;
    var fill=function(){
      var el=d.getElementById('deviceIp');
      if(!el){setTimeout(fill,120);return;}
      el.value=location.hostname;
      el.dispatchEvent(new Event('change',{bubbles:true}));
      say('running from repo ('+(html.length/1024|0)+' KB) - device '+location.hostname);
    };
    fr.style.display='block';fill();
  }catch(e){
    // ⚠️ Name the likely cause. Without the network this page is empty and the
    // on-device IDE is the answer -- but only if the reader is told it exists.
    say(e.message+' - no internet in this browser? The IDE on the device '
        +'(/tinyc_ide.html.gz, if uploaded) works offline.',true);
  }
})();
</script></body></html>)XX";

// GET /tcrepo — hand out the loader. Privileged like every other TinyC handler:
// what it loads can overwrite a slot.
static void HandleTinyCRepoIde(void) {
  if (!HttpCheckPriviledgedAccess()) { return; }
  Webserver->send_P(200, PSTR("text/html"), TC_REPO_IDE_PAGE);
}

#endif  // USE_TINYC_REPO_IDE
#endif  // _XDRV_124_TINYC_REPOIDE_H_
