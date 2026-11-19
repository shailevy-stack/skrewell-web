class SkrewellProc extends AudioWorkletProcessor {
  constructor(o) {
    super();
    this.ready = false; this.q = []; this.run = false; this.gain = 0; this.vol = 0.8;
    this.sbuf = new Float32Array(4 * 64 * 16); this.sn = 0;
    this.port.onmessage = (e) => this.ready ? this.cmd(e.data) : this.q.push(e.data);
    WebAssembly.instantiate(o.processorOptions.bytes, {}).then((r) => {
      this.w = r.instance.exports;
      this.w.init(sampleRate);
      this.ready = true;
      for (const m of this.q) this.cmd(m);
      this.q = [];
    });
  }
  dv() { return new DataView(this.w.memory.buffer, this.w.patch_ptr(), this.w.patch_size()); }
  cmd(m) {
    const w = this.w;
    switch (m.c) {
      case 'run': this.run = m.on; break;
      case 'vol': this.vol = m.x; break;
      case 'pf': this.dv().setFloat32(m.off, m.x, true); break;
      case 'pm': w.set_mode(m.m); break;
      case 'patch': new Uint8Array(w.memory.buffer, w.patch_ptr(), w.patch_size()).set(m.bytes); break;
      case 'verb': w.set_verb(m.a[0], m.a[1], m.a[2], m.a[3], m.a[4], m.a[5], m.a[6]); break;
      case 'glue': w.set_glue(m.a[0], m.a[1], m.a[2], m.a[3], m.a[4]); break;
    }
  }
  process(ins, outs) {
    const o = outs[0], L = o[0], R = o[1] || o[0], n = L.length;
    if (!this.ready || (!this.run && this.gain < 1e-4)) { this.gain = 0; L.fill(0); if (R !== L) R.fill(0); return true; }
    const w = this.w;
    w.render(n);
    const mem = w.memory.buffer;
    const oL = new Float32Array(mem, w.out_l(), n), oR = new Float32Array(mem, w.out_r(), n);
    const tgt = this.run ? 1 : 0;
    for (let i = 0; i < n; i++) {
      this.gain += (tgt - this.gain) * 0.002;
      const g = this.gain * this.vol;
      L[i] = oL[i] * g; if (R !== L) R[i] = oR[i] * g;
    }
    const sc = new Float32Array(mem, w.scope_ptr(), n * 16);
    for (let i = 0; i < n; i += 2) { this.sbuf.set(sc.subarray(i * 16, i * 16 + 16), this.sn); this.sn += 16; }
    if (this.sn >= this.sbuf.length) {
      this.port.postMessage({ c: 'scope', pts: this.sbuf, g: w.glue_gain() }, [this.sbuf.buffer]);
      this.sbuf = new Float32Array(4 * 64 * 16); this.sn = 0;
    }
    return true;
  }
}
registerProcessor('skrewell', SkrewellProc);
