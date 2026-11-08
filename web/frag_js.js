/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Audi Étoffe
 * See the LICENSE file in the top directory. */
<script>
(function () {
  'use strict';
  var $ = function (id) { return document.getElementById('xdjp-' + id); };
  var spec = null;
  var libSrc = document.getElementById('xdjp-lib').textContent;
  var input = null, stockKnown = false, busy = false, objectUrl = null;
  var statusEl = $('status');
  window.addEventListener('error', function (e) { showBad('Something went wrong on this page: ' + (e.message || 'unknown error')); });
  function showBad(m) { statusEl.hidden = false; statusEl.className = 'status bad'; statusEl.textContent = m; }
  async function loadSpec() {
    var bin = atob(document.getElementById('xdjp-spec').textContent.trim()), bytes = new Uint8Array(bin.length);
    for (var i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
    if (typeof DecompressionStream === 'undefined') throw new Error('This browser is too old. Please use a current version of Chrome, Edge, Firefox or Safari.');
    var stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
    return JSON.parse(await new Response(stream).text());
  }
  function boot() {
  if (typeof XDJPatch === 'undefined') { showBad('The patcher code did not load completely (your site may have cut the page short).'); return; }

  function note(el, cls, text) { el.textContent = ''; if (!text) return; var d = document.createElement('div'); d.className = 'note ' + cls; d.textContent = text; el.appendChild(d); }
  function fmtMB(n) { return (n / 1048576).toFixed(1) + ' MB'; }
  function setBusy(b) { busy = b; $('go').disabled = b || !input; $('file').disabled = b; $('optDjp').disabled = b || (input && !stockKnown); $('optHires').disabled = b; }

  async function onFile(f) {
    note($('msg'), '', ''); $('result').hidden = true; input = null; stockKnown = false; $('go').disabled = true;
    if (!f) return;
    $('fileInfo').hidden = false; $('fileInfo').textContent = f.name + ' \u00b7 ' + fmtMB(f.size);
    if (f.size < 20e6 || f.size > 40e6) { note($('fileNote'), 'err', 'This does not look like the XDJ-1000MK2 update (it should be about 24 MB).'); return; }
    note($('fileNote'), '', ''); $('fileNote').textContent = 'Checking the file\u2026';
    var buf;
    try { buf = new Uint8Array(await f.arrayBuffer()); } catch (e) { note($('fileNote'), 'err', 'Could not read that file.'); return; }
    var sha = await XDJPatch.sha256hex(buf);
    input = buf; stockKnown = (sha === spec.stockSha256);
    if (stockKnown) {
      note($('fileNote'), 'ok', 'Recognised: the original XDJ-1000MK2 1.45 update.');
    } else {
      note($('fileNote'), 'warn', 'This is not the unmodified 1.45 file. The patcher will check whether it can still use it (for example, a firmware that already has the extra features) and tell you if not.');
    }
    $('optDjp').disabled = !stockKnown;
    setBusy(false);
  }

  $('file').addEventListener('change', function (e) { onFile(e.target.files[0]); });
  if ($('ver')) $('ver').textContent = spec.verTo;
  statusEl.hidden = true;                           // everything loaded: hide the starting message
  var drop = $('drop');
  ['dragenter', 'dragover'].forEach(function (ev) { drop.addEventListener(ev, function (e) { e.preventDefault(); drop.classList.add('over'); }); });
  ['dragleave', 'drop'].forEach(function (ev) { drop.addEventListener(ev, function (e) { e.preventDefault(); drop.classList.remove('over'); }); });
  drop.addEventListener('drop', function (e) { var f = e.dataTransfer && e.dataTransfer.files[0]; if (f) onFile(f); });

  var workerGlue = "self.onmessage=async function(e){var d=e.data;try{var r=await XDJPatch.patchFirmware(d.input,d.spec,d.opts,function(f,t){self.postMessage({type:'p',f:f,t:t});});self.postMessage({type:'done',bytes:r.bytes,layout:r.layout,kind:r.kind,inputSha:r.inputSha,outputSha:r.outputSha},[r.bytes.buffer]);}catch(err){self.postMessage({type:'err',msg:(err&&err.userMessage)||('Unexpected error: '+(err&&err.message))});}};";

  function progress(f, t) { $('barFill').style.width = Math.round(f * 100) + '%'; $('progressText').textContent = t || ''; }
  function finish(r) {
    if (objectUrl) URL.revokeObjectURL(objectUrl);
    objectUrl = URL.createObjectURL(new Blob([r.bytes], { type: 'application/octet-stream' }));
    $('dl').href = objectUrl;
    $('resultInfo').textContent = 'Built from: ' + r.kind + ' \u00b7 ' + fmtMB(r.bytes.length) + ' \u00b7 SHA-256 ' + r.outputSha;
    $('result').hidden = false; $('progress').hidden = true; setBusy(false);
    note($('msg'), 'ok', 'Done. The result was decompressed again and checked against what was built.');
  }
  function fail(msg) { $('progress').hidden = true; setBusy(false); note($('msg'), 'err', msg); }

  $('go').addEventListener('click', function () {
    if (!input || busy) return;
    note($('msg'), '', ''); $('result').hidden = true; $('progress').hidden = false; progress(0, 'Starting\u2026'); setBusy(true);
    var opts = { djpatcher: $('optDjp').checked && stockKnown, hires: $('optHires').checked };
    var data = input.slice();                       // keep the original selection intact for another run
    var worker = null;
    try { worker = new Worker(URL.createObjectURL(new Blob([libSrc + '\n' + workerGlue], { type: 'text/javascript' }))); } catch (e) { worker = null; }
    if (worker) {
      worker.onmessage = function (e) {
        var m = e.data;
        if (m.type === 'p') progress(m.f, m.t);
        else if (m.type === 'done') { worker.terminate(); finish({ bytes: m.bytes, kind: m.kind, layout: m.layout, outputSha: m.outputSha }); }
        else if (m.type === 'err') { worker.terminate(); fail(m.msg); }
      };
      worker.onerror = function () { worker.terminate(); fail('The browser stopped the patcher (usually not enough memory). Close other tabs and try again, or use a desktop browser.'); };
      worker.postMessage({ input: data, spec: spec, opts: opts }, [data.buffer]);
    } else {                                         // no worker available: run on the page (the progress bar still updates)
      XDJPatch.patchFirmware(data, spec, opts, progress).then(finish, function (err) { fail((err && err.userMessage) || ('Unexpected error: ' + (err && err.message))); });
    }
  });
  }
  loadSpec().then(function (s) { spec = s; boot(); }, function (e) { showBad((e && e.message) || 'The patcher data could not be loaded.'); });
})();
</script>