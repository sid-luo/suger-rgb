const e=async t=>{
  let o;
  try {
    o=await navigator.serial.requestPort();
  } catch (error) {
    if(error.name!=="NotFoundError") { alert(`Error: ${error.message}`); return; }
    try {
      const dialog=await import("./index-Dt9w-IG1.js");
      dialog.openNoPortPickedDialog(()=>e(t));
    } catch(error) { console.error(error); alert(`Error: ${error.message}`); }
    return;
  }
  if(!o)return;
  try {
    // Load the same canonical module URL used by the chip modules before opening the port.
    await import("./install-dialog-im156JnI.js");
    await o.open({baudRate:115200,bufferSize:8192});
  } catch(error) { console.error(error); alert(`Error: ${error.message}`); return; }
  const n=document.createElement("ewt-install-dialog");
  n.port=o;
  n.manifestPath=t.manifest||t.getAttribute("manifest");
  n.overrides=t.overrides;
  n.addEventListener("closed",async()=>{
    try { if(o.readable||o.writable)await o.close(); }
    catch(error) { console.error("Failed to close serial port",error); }
  },{once:true});
  document.body.appendChild(n);
};class t extends HTMLElement{connectedCallback(){if(this.renderRoot)return;if(this.renderRoot=this.attachShadow({mode:"open"}),!t.isSupported||!t.isAllowed)return this.toggleAttribute("install-unsupported",!0),void(this.renderRoot.innerHTML=t.isAllowed?"<slot name='unsupported'>Your browser does not support installing things on ESP devices. Use Mozilla Firefox, Google Chrome or Microsoft Edge.</slot>":"<slot name='not-allowed'>You can only install ESP devices on HTTPS websites or on the localhost.</slot>");this.toggleAttribute("install-supported",!0);const o=document.createElement("slot");o.addEventListener("click",(async t=>{t.preventDefault(),e(this)})),o.name="activate";const n=document.createElement("button");if(n.innerText="Connect",o.append(n),"adoptedStyleSheets"in Document.prototype&&"replaceSync"in CSSStyleSheet.prototype){const e=new CSSStyleSheet;e.replaceSync(t.style),this.renderRoot.adoptedStyleSheets=[e]}else{const e=document.createElement("style");e.innerText=t.style,this.renderRoot.append(e)}this.renderRoot.append(o)}}t.isSupported="serial"in navigator,t.isAllowed=window.isSecureContext,t.style='\n  button {\n    position: relative;\n    cursor: pointer;\n    font-size: 14px;\n    font-weight: 500;\n    padding: 10px 24px;\n    color: var(--esp-tools-button-text-color, #fff);\n    background-color: var(--esp-tools-button-color, #03a9f4);\n    border: none;\n    border-radius: var(--esp-tools-button-border-radius, 9999px);\n  }\n  button::before {\n    content: " ";\n    position: absolute;\n    top: 0;\n    bottom: 0;\n    left: 0;\n    right: 0;\n    opacity: 0.2;\n    border-radius: var(--esp-tools-button-border-radius, 9999px);\n  }\n  button:hover::before {\n    background-color: rgba(255,255,255,.8);\n  }\n  button:focus {\n    outline: none;\n  }\n  button:focus::before {\n    background-color: white;\n  }\n  button:active::before {\n    background-color: grey;\n  }\n  :host([active]) button {\n    color: rgba(0, 0, 0, 0.38);\n    background-color: rgba(0, 0, 0, 0.12);\n    box-shadow: none;\n    cursor: unset;\n    pointer-events: none;\n  }\n  .hidden {\n    display: none;\n  }',customElements.define("esp-web-install-button",t);
