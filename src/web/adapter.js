/* Runs inside the pinned renderer's closure. Its DOM/CSS stays authoritative. */
(function desktopLobby() {
  "use strict";
  let profile = null, lastLN = null, panel = null, loginPending = false;
  let renderedLobbyKey="";
  let returningToLobby=false;
  let web3=false;
  let hideGuestJoukyuuTables=true;
  const selectedMatches=new Set();
  let restoringMatchUI=false;
  // Let Windows remain the only persistent account store. The official login
  // renderer can read/write its account key, but this proxy keeps it in RAM.
  const durableStorage=E;let volatileAccount="";
  E=new Proxy(durableStorage,{
    get(target,key){
      if(key===C.ha)return volatileAccount;
      if(key==="setItem")return (name,value)=>name===C.ha?(volatileAccount=String(value)):target.setItem(name,value);
      if(key==="removeItem")return name=>name===C.ha?(volatileAccount=""):target.removeItem(name);
      const value=Reflect.get(target,key,target);return typeof value==="function"?value.bind(target):value;
    },
    set(target,key,value){if(key===C.ha)volatileAccount=String(value);else target[key]=value;return true;}
  });
  const bridge = window.chrome && window.chrome.webview;
  const send = (command, value = "") => {
    if (bridge) bridge.postMessage(command + "\n" + String(value));
    else window.dispatchEvent(new CustomEvent("tenhou-native-command", {detail: {command, value}}));
  };
  const registration=new Map();let registrationSequence=0,selectedRoom="L0000";
  // Reuse the official registration dialogs and the Windows registration
  // service. Only IO is replaced; account creation still requires its second
  // explicit confirmation, and the resulting ID is held in RAM.
  eb=function(url,callback){
    const request=new URL(url);
    if(request.pathname!=="/regid")throw Error("Unexpected web lobby HTTP request");
    const sequence=++registrationSequence;registration.set(sequence,callback);
    send("register",sequence+"\n"+(request.searchParams.has("q")?"query":"create")+"\n"+request.searchParams.get("uname"));
  };
  U.ERR=Nb.ERR;
  function room(code){
    code=code.trim().toUpperCase();if(/^\d{4}$/.test(code))code="L"+code;if(/^\d{8}$/.test(code))code="C"+code;
    if(!/^(L\d{4}|C\d{8})$/.test(code))return false;
    selectedRoom=code;send("room",code);S.B=code[0]==="L"?Number(code.slice(1)):10000+Number(code.slice(1,5));
    if(!profile)renderWelcome();return true;
  }
  xa.addEventListener("click",event=>{
    const link=event.target.closest("a[href]");if(!link)return;
    const match=link.getAttribute("href").match(/^\?(L\d{4}|C\d{8})$/);
    if(match){event.preventDefault();room(match[1]);}
  },true);
  R.c65=function(){Gb.O(L(F.Eh)+"<br><small>L0123 / C01234567</small><br><input name=desktopRoom type=text size=12 value='"+selectedRoom+"' />",3,dialog=>room(dialog.querySelector("input[name=desktopRoom]").value));};
  function error(message, resetJoin=false) { if(resetJoin){selectedMatches.clear();if(profile)render();}if(!profile){loginPending=false;kc.Vc(false);}Gb.O(oa(message), 1); }
  // Only explicit lobby actions cross this boundary. Native protocol messages
  // (including REJOIN) remain owned by the original client's state machine.
  Yb.S = function (packet) {
    if (typeof packet === "string") {
      const match = packet.match(/^<PXR [vV]="(-?\d+)"\s*\/>$/);
      if (match) send("rank", match[1]);
      return;
    }
    if (packet.tag === "JOIN") {
      // Replaying the pinned renderer's selection restores its private UI
      // state after Rd(), but must never submit a second native reservation.
      if(restoringMatchUI)return;
      if(packet.t){const type=Number(packet.t.split(",")[1]);if(Number.isInteger(type)&&type>=0&&type<=32767)selectedMatches.add(type);}
      else selectedMatches.clear();
      // The upstream handler has already changed these buttons in place.
      // A later identical snapshot should keep that same selection DOM.
      if(renderedLobbyKey)renderedLobbyKey=lobbyRenderKey();
      send(packet.t ? "join" : "cancel", packet.t ? packet.t.split(",")[1] : "");
    }
    else if (packet.tag === "CHAT") send("chat", packet.text || "");
  };
  Yb.Z = Yb.Bf = function () {};
  Yb.O = function () { throw Error("The lobby uses the Windows connection"); };
  // The original timer checks this property; zero keeps web polling inactive.
  Yb.Fb = 0;
  // /4 normally schedules modal fades on its game animation loop. The desktop
  // lobby has no web table loop, so retain those timings with browser timers.
  kb=function(callback,delay){if(delay>0)return String(B.setTimeout(callback,delay));callback();return "";};
  lb=function(timer){B.clearTimeout(Number(timer));};
  R.c23 = function () { renderWelcome();send("logout"); };
  // /4 also uses c9 for internal navigation. It must not disconnect Windows.
  R.c9 = function () { if(profile)render();else renderWelcome(); };
  R.c5 = function (url) {
    if (typeof url === "string" && url) return send("open", url);
    Gb.O(L(F.kh)+"<br><input type=text name=desktopURL size=20 value='' style='font-size:100%' />", 3, function (dialog) {
      const value=dialog.querySelector("input[name=desktopURL]").value.trim();
      if(!value)return false;
      send("open", value);
    });
  };
  R.c25 = function () { send("browser", "https://tenhou.net/man/"); };
  R.c26 = function () { send("browser", "https://tenhou.net/2/"); };
  R.c27 = function () { send("subscription"); };
  Xb.logOpen = Xb.wgOpen = function (event) { event.preventDefault(); send("open", event.currentTarget.href); return false; };
  let press=null,suppressCopyClick=null;
  function cardLink(target){const link=target.closest?.("#c31 a[href],#c32 a[href]");if(!link)return null;try{const url=new URL(link.href);return url.hostname==="tenhou.net"&&(url.searchParams.has("wg")||url.searchParams.has("log"))?link:null;}catch(_){return null;}}
  const originalAction=R.s7;
  R.s7=function(event){
    const link=event.target.closest?.("a[href]");
    // /4's generic s7 handler runs before card onclick/copy handling. These
    // links have dedicated desktop handlers, so avoid opening them twice.
    if(cardLink(event.target)||(link&&/^\?(L\d{4}|C\d{8})$/.test(link.getAttribute("href"))))return;
    return originalAction.apply(this,arguments);
  };
  function cancelPress(){if(press)B.clearTimeout(press.timer);press=null;}
  xa.addEventListener("contextmenu",event=>{const link=cardLink(event.target);if(!link)return;event.preventDefault();event.stopImmediatePropagation();if(!suppressCopyClick)send("copy",link.href);cancelPress();},true);
  xa.addEventListener("pointerdown",event=>{
    cancelPress();suppressCopyClick=null;const link=cardLink(event.target);if(!link||event.button!==0)return;
    press={x:event.clientX,y:event.clientY,timer:B.setTimeout(()=>{suppressCopyClick=link.href;send("copy",link.href);press=null;},600)};
  },true);
  xa.addEventListener("pointermove",event=>{if(press&&Math.hypot(event.clientX-press.x,event.clientY-press.y)>8)cancelPress();},true);
  for(const event of ["pointerup","pointercancel","scroll"])xa.addEventListener(event,cancelPress,true);
  xa.addEventListener("click",event=>{if(suppressCopyClick&&cardLink(event.target)){event.preventDefault();event.stopImmediatePropagation();suppressCopyClick=null;}},true);
  let watchSequence=0;
  const watchCache=new Map();
  Tc.$f = function () {
    const cached=watchCache.get(S.B);if(cached)Tc.dg(cached);
    send("watch",++watchSequence);
  };
  // Windows fetches the same public HTTPS list as /4; the official renderer
  // receives its CSV rows without an old embedded-browser/HTML round trip.
  const originalWatch = Tc.dg;
  function normalizeWatchFlags(value){
    const flags=Number(value)&127,players=flags&3,winds=flags&12,tables=flags&112;
    return (players||3)|(winds||12)|([16,32,64,112].includes(tables)?tables:64);
  }
  Tc.dg=function(games){
    if(!A("c31"))return;
    // The pinned /4 lookup has an unbounded loop for invalid group values.
    // Private rooms omit the table group while rendering: retain it separately
    // so returning to the public lobby cannot persist/reload the invalid 15.
    Tc.flags=normalizeWatchFlags(Tc.flags);const tables=Tc.flags&112;
    originalWatch(games);
    if(S.B!==0)Tc.flags=(Tc.flags&15)|tables;
    send("watchFilters",Tc.flags);
  };
  function parseWatchList(source){
    const match=source.match(/^\s*sw\((\[[\s\S]*\])\);?\s*$/);
    if(!match)throw Error("Invalid watch response");
    const rows=JSON.parse(match[1]);
    if(!Array.isArray(rows)||rows.length>2000)throw Error("Invalid watch rows");
    return rows.map(row=>{
      if(typeof row!=="string")throw Error("Invalid watch row");
      const game=row.split(",");
      if(![13,16].includes(game.length)||!/^[0-9a-f]{8}$/i.test(game[0])||!/^\d+$/.test(game[1])||!/^\d{2}:\d{2}$/.test(game[2])||!/^\d{1,5}$/.test(game[3]))throw Error("Invalid watch game");
      for(let seat=0;seat<4;seat++){
        const at=4+seat*3;
        if(!game[at])continue;
        if(!/^[A-Za-z0-9+/]+={0,2}$/.test(game[at])||!/^\d{1,2}$/.test(game[at+1])||!/^\d+(?:\.\d+)?$/.test(game[at+2]))throw Error("Invalid watch player");
        // The upstream template inserts decoded names into HTML. Escape only
        // that text; never execute or interpolate the response's script wrapper.
        const name=decodeURIComponent(escape(atob(game[at])));
        game[at]=btoa(unescape(encodeURIComponent(oa(name))));
      }
      return game;
    });
  }
  function receiveWatch(message){
    if(!profile||message.sequence!==watchSequence||message.room!==S.B)return;
    try{
      if(message.error)throw Error("Watch request failed");
      const games=parseWatchList(message.source);watchCache.set(S.B,games);Tc.dg(games);
    }catch(_){
      if(watchCache.has(S.B))return;
      const list=A("c31");if(list)list.textContent=rb.Od()==="zh"?"无法加载观战列表，请重新选择观战页重试。":rb.Od()==="ja"?"観戦リストを取得できませんでした。観戦タブを選び直してください。":"Unable to load games. Select the Watch tab again to retry.";
    }
  }
  const originalLogs = Uc.tj,originalAccountLogs=Uc.Wi;
  let historyLogs=[],fileLogs=[],fileLogSequence=0,accountLogMode=false,accountResponse=null,accountLogError=false,accountRequestSequence=0;
  function accountLogButton(){
    const link=A("c32")?.querySelector("a[href='https://tenhou.net/mjlog.html']");
    if(link)link.onclick=function(){
      accountLogMode=!accountLogMode;accountResponse=null;accountLogError=false;
      const sequence=++accountRequestSequence;renderLogs();
      if(accountLogMode)send("accountLogs",sequence);else send("logs");
      return false;
    };
  }
  function renderLogs() {
    const list=A("c32");if(!list)return;
    if(accountLogMode){
      if(accountResponse!==null)originalAccountLogs(list,accountResponse);
      else{
        list.innerHTML="";
        Ea("div",list,"",{innerHTML:"<a class=bt3 href='https://tenhou.net/mjlog.html' target=_blank style='padding:1em 0;'>"+L(F.Yg)+"</a>"},{textAlign:"center"});
        Ea("div",list,"gray",{innerHTML:L(F.Zg)},{fontSize:"75%",padding:"0.5em 0"});
        Ea("div",list,"gray",{textContent:accountLogError?(rb.Od()==="zh"?"无法查询该账号的牌谱，请稍后重试。":rb.Od()==="ja"?"牌譜を取得できませんでした。後ほど再度お試しください。":"Unable to retrieve this account’s replays. Please try again later."):"L O A D I N G ..."},{padding:"1em 0"});
      }
    }else originalLogs();
    accountLogButton();
    if((!accountLogMode||accountResponse!==null)&&!list.querySelector("a[href*='?log=']"))Ea("div",list,"gray",{textContent:rb.Od()==="zh"?"没有可查看的牌谱":rb.Od()==="ja"?"閲覧可能な牌譜がありません":"No replays available."},{textAlign:"center",padding:"1em"});
  }
  Uc.tj = function () { if (A("c32")) { renderLogs(); send("logs"); } };
  for (const lang of Object.keys(window.tenhouLanguages)) rb.assign(lang, window.tenhouLanguages[lang]);
  const lobbyLanguages=["ja","en","zh"];
  for(const lang of Object.keys(rb.hb))if(!lobbyLanguages.includes(lang))delete rb.hb[lang];
  rb.Od(lobbyLanguages.includes(E[C.Me])?E[C.Me]:"zh");
  Hb();
  function resize() {
    // Same 4:3 viewport, resolution quantization and em scale as T.ej in /4.
    cb = B.devicePixelRatio || 1;
    const d = B.innerWidth * cb, c = B.innerHeight * cb;
    let k, m, b, l;
    if (3*d < 4*c) {
      k = Math.max(800, Math.min(2640, 20*Math.floor(d/20))); m = 3*k/4;
      b = d >= 800 && d < 2660 ? k : d; l = 3*b/4;
    } else {
      m = Math.max(600, Math.min(1980, 15*Math.floor(c/15))); k = 4*m/3;
      l = c >= 600 && c < 1995 ? m : c; b = 4*l/3;
    }
    for (const element of [T.Bb, T.la]) Object.assign(element.style, {
      display: "", opacity: "1", width: Db(b), height: Db(l),
      transform: "translate("+Db(Math.floor((d-b)/2))+","+Db(Math.floor((c-l)/2))+")"
    });
    Ha.style.fontSize = k/cb/50 + "px";
    T.Ib=k;T.Nc=m;
  }
  Object.assign(T.Bb.style, {background: "url('assets/background-overlay.png') center center / cover no-repeat,url('assets/background.jpg') center center / cover no-repeat"});
  // Do not display or initialize any of the original web table canvases.
  for (const element of [T.Cb, T.Rc, T.Vf, T.ca.canvas, T.Lf.canvas, T.fc]) element.style.display = "none";
  // Use the upstream six-tab settings menu. Native-only options continue to
  // live in Windows settings; supported common controls use the same INI keys.
  const backgroundMask=Ea("div",T.Bb,"nopp",{}, {position:"absolute",inset:"0",background:"url('assets/background-overlay.png') center / cover no-repeat"});
  backgroundMask.style.display="none";
  bc.Dd=function(){
    const hsv=bc.Nd(),mask=bc.Md(),custom=Lc();
    T.Bb.style.background=custom?"url('"+E[C.eb].replace(/['\\\r\n]/g,"")+"') center / cover no-repeat":"url('assets/background.jpg') center / cover no-repeat";
    T.Bb.style.filter="hue-rotate("+Number(hsv.slice(0,3))+"deg) saturate("+(Number(hsv.slice(3,6))/100)+") brightness("+(Number(hsv.slice(6,9))/100*(1-Number(mask.slice(3,6))/125))+")";
    backgroundMask.style.display="";backgroundMask.style.opacity=String(Math.min(1,Number(mask.slice(0,3))/80));
    backgroundMask.style.backgroundColor="rgba(0,0,0,"+Math.max(0,(Number(mask.slice(0,3))-80)/100)+")";
  };
  function settings(){
    lc.jb();
    A("c66").addEventListener("click",()=>send("options"));
    const zh=rb.Od()==="zh";
    if(zh){
      A("c48").nextElementSibling.textContent="显示玩家信息";
    }
    T.Qi.querySelector("button[name=c15]").textContent=zh?"窗口最大化／还原":rb.Od()==="ja"?"最大化／元に戻す":"Maximize / restore";
    // These controls configure the native table, rather than a hidden web table.
    for(const id of ["c67","c68","c69","c70","c71","c72","c73","c74","c58"]){
      A(id).onchange=()=>{if(id==="c58")C.ob(C.gd,Number(A(id).checked),0);send("option",id+"\n"+Number(A(id).checked));};
    }
    // Match the native FLAGS0 default while waiting for its saved preferences.
    A("c69").checked=true;A("c70").checked=true;
    A("c71").checked=false;A("c72").checked=false;
    // Windows exposes tile artwork through its own renderer.
    // Keep their source controls visible with an explicit native-settings route.
    for(const id of ["c48"]){A(id).disabled=true;A(id).checked=false;A(id).nextElementSibling.style.opacity=".4";A(id).nextElementSibling.title=zh?"Windows 牌桌没有对应的可配置选项；原版选项可从标题栏右键设置打开":"Unavailable in the Windows table; use the title-bar Windows settings menu";}
    for(const id of ["c49R","c50G","c51B"]){A(id).disabled=true;A(id).title=zh?"原生牌面请从标题栏右键的设置修改":"Use Windows settings in the title-bar menu for native tile artwork";}
    Ea("div",A("c49R").closest("div.tab1"),"desc",{textContent:zh?"标题栏右键 → 设置。":rb.Od()==="ja"?"タイトルバーを右クリック → 設定。":"Right-click the title bar → Settings."},{marginTop:"1em"});
    R.c53=event=>{
      const index=[...event.target.parentNode.children].indexOf(event.target);C.ob(C.Gb,index,5);
      volumeLabel(index);send("volume",index);
    };
    R.c15=()=>send("fullscreen");
    send("options");
  }
  function volumeLabel(value){A("c53").innerHTML=Q(L(F.sf)+":")+value;for(const [index,button]of [...A("c53").nextElementSibling.children].entries())button.classList.toggle("s4",index===Number(value));}
  settings();
  if(E[C.eb]||E[C.uc])bc.Dd();
  R.c55 = function (event) { rb.Od(event.target.innerText.substr(0,2));settings();if(profile)render();else renderWelcome(); };
  function newPanel(){
    if(panel)panel.remove();
    panel=Ea("div",T.la,"s0",{},{position:"absolute",background:"rgba(0,0,0,0.90)",border:"1px solid #444",padding:"0.5em",left:"50%",top:"50%",transform:"translate(-50%,-50%) scale(1.0)"});
    return panel;
  }
  function renderWelcome(data={}){
    renderedLobbyKey="";
    returningToLobby=false;selectedMatches.clear();
    profile=null;lastLN=null;loginPending=false;watchCache.clear();++watchSequence;
    historyLogs=[];fileLogs=[];++fileLogSequence;
    accountLogMode=false;accountResponse=null;accountLogError=false;++accountRequestSequence;
    if(data.account!==undefined)E[C.ha]=/^ID[0-9A-F]{8}-[0-9A-Za-z]{8}$/.test(data.account)?data.account:"";
    if(data.sex)E[C.Fc]=data.sex;
    kc.Rd(newPanel());
    panel.querySelector("button[name=guestok]").onclick=function(){startLogin("NoName");};
    panel.querySelector("button[name=c6]").onclick=function(){startLogin(E[C.ha]||"NoName");};
    kc.Vc(false);
    updateWelcomeRoom();
    send("rendered","welcome");
  }
  function startLogin(account){
    if(loginPending)return;
    loginPending=true;kc.Vc(true);send("login",account+"\n"+(E[C.Fc]||"M"));
  }
  function updateWelcomeRoom(){
    if(profile||!panel)return;
    const label=panel.querySelector("img")?.nextElementSibling;
    if(label){label.id="desktopRoomLabel";label.innerHTML=selectedRoom==="L0000"?"":Q(L(selectedRoom[0]==="L"?F.Se:F.Re))+selectedRoom.slice(0,5);}
    if(A("c64"))A("c64").textContent=L(selectedRoom==="L0000"?F.th:F.uh);
  }
  function lobbyRenderKey(){
    return JSON.stringify([web3,hideGuestJoukyuuTables,S.B,rb.Od(),Object.keys(profile).filter(name=>name!=="tag").sort().map(name=>[name,profile[name]]),[...selectedMatches].sort((a,b)=>a-b)]);
  }
  function renderEntryRequirements(){
    if(S.B!==0)return;
    const days=profile.expiredays===undefined?(profile.expire&&profile.expire!=="0"?1:0):Number(profile.expiredays)||0;
    // PF4/PF3 use 9 for 1 kyu, 13 for 4 dan, and 16 for 7 dan.
    const stats=[Z[11]||[],Z[10]||[]].map(values=>({rank:Number(values[0])||0,rating:Number(values[2])||0}));
    const allowed=[
      web3||(days>=1&&stats.some(p=>!(p.rank>=13&&p.rating>=1800))),
      !(hideGuestJoukyuuTables&&/^noname$/i.test(Z[0]))&&(web3||stats.some(p=>!(p.rank>=16&&p.rating>=2000)&&((p.rank>=9&&days>=1)||days>=60))),
      (web3||days>=1)&&stats.some(p=>p.rank>=13&&p.rating>=1800)
    ];
    // The pinned renderer already applies Web eligibility to individual
    // buttons, including Phoenix membership and opt-gated rules. Keep the
    // first two Web tables visible unless the guest Advanced option applies.
    for(const [tier,key] of [227,223,224].entries()){
      const section=panel.querySelector(".tabs>.tab0.tabs>div.tab"+tier);
      const table=section?.querySelector("table.jtab");if(!table)continue;
      let notice=section.querySelector(":scope > .gray");
      if(!notice)notice=Ea("div",section,"gray",{},{position:"absolute",left:"30%",top:"50%",width:"50%",transform:"translate(-50%,-50%)"});
      notice.dataset.desktopEntry=String(tier);notice.textContent=L(key);
      table.style.display=allowed[tier]?"":"none";
      notice.style.display=allowed[tier]?"none":"";
    }
  }
  function render() {
    if (!profile) return;
    // Replayed native snapshots and return notifications often describe the
    // same lobby. Keep its DOM, scroll positions and loaded cards in place.
    const key=lobbyRenderKey();
    if(key===renderedLobbyKey)return;
    newPanel();
    Z.Rd(panel, profile);
    renderEntryRequirements();
    // Stats are aggregate PF4/PF3 records. Qualification explanation entries
    // must select that game's aggregate too, rather than leaving an old sanma
    // selection on screen after a four-player entry is clicked.
    const originalSelection=R.c20;
    if(!originalSelection.desktopStats){
      const select=function(event){
        const match=event.target.id.match(/^c20-rule4-(\d+)$/);
        if(match){const type=Number(match[1]);if(type&&type!==65536&&!(type&1)){const target={id:"c20-rule4-"+(type|1)};return originalSelection({target});}}
        return originalSelection(event);
      };select.desktopStats=true;R.c20=select;
    }
    for(const radio of panel.querySelectorAll("input[name^=wg]"))radio.addEventListener("change",()=>Tc.dg());
    panel.querySelector("input[name=tg0].tab3")?.addEventListener("change",event=>{if(event.target.checked)Tc.$f();});
    panel.querySelector("input[name=tg0].tab4")?.addEventListener("change",()=>send("logs"));
    restoringMatchUI=true;
    try{for(const type of selectedMatches)R.c18(null,String(type));}
    finally{restoringMatchUI=false;}
    if (lastLN) Z.dh(lastLN);
    renderedLobbyKey=key;
    send("rendered","lobby");
  }
  function readEvent(xml){
    const document=new DOMParser().parseFromString(xml,"text/xml");
    if(document.querySelector("parsererror"))return null;
    const root=document.documentElement,data={tag:root.tagName};
    for(const attr of root.attributes)data[attr.name]=attr.value;
    return data;
  }
  function updateProfile(data){
    if(!profile){const tabs=ka(E[C.Ad]);tabs[0]=0;E[C.Ad]=tabs.join(",");}
    loginPending=false;profile=Object.assign(profile||{},data);
  }
  function updateLobby(message){
    if(S.B!==message.id)selectedMatches.clear();
    S.B=message.id;selectedRoom=message.code||"L"+String(message.id).padStart(4,"0");
    if(message.watchFlags!==undefined)Tc.flags=normalizeWatchFlags(message.watchFlags);
  }
  function receive(message) {
    if(typeof message.web3==="boolean")Z.desktopWeb3=web3=message.web3;
    if(typeof message.hideGuestJoukyuuTables==="boolean")hideGuestJoukyuuTables=message.hideGuestJoukyuuTables;
    if(message.type==="startup"){
      // DOM creation precedes local image/font decode. Keep the native loading
      // frame until the completed login page has had two animation frames.
      const urls=new Set([...document.images].map(image=>image.currentSrc||image.src));
      for(const element of [T.Bb,backgroundMask]){
        const background=B.getComputedStyle(element).backgroundImage;
        for(const match of background.matchAll(/url\(["']?(.*?)["']?\)/g))urls.add(match[1]);
      }
      const images=[...urls].filter(url=>{try{const parsed=new URL(url,location.href);return parsed.origin===location.origin||parsed.protocol==="data:";}catch(_){return false;}}).map(url=>{
        const image=new Image();image.src=url;return image.decode();
      });
      void (async()=>{
        await Promise.allSettled(images);
        // Trigger font selection for the final synchronized native login data.
        void document.body.offsetHeight;
        await document.fonts.ready;
        B.requestAnimationFrame(()=>B.requestAnimationFrame(()=>send("startupRendered",message.sequence)));
      })();
      return;
    }
    if(message.type==="nativeReturn"){
      // The native score/certificate windows own this part of the return.
      // Keep only the desktop background until their OK flow has completed.
      T.la.style.visibility=message.active?"hidden":"";
      T.la.inert=!!message.active;
      return;
    }
    if(message.type==="snapshot"){
      // A synchronization is one browser task: merge every profile before
      // rendering, then apply the small live updates to that completed panel.
      updateLobby(message);
      const records=message.events.map(xml=>({xml,data:readEvent(xml)})).filter(record=>record.data);
      for(const {data} of records)if((data.tag==="HELO"||data.tag==="PROF")&&(data.PF4||data.PF3||data.uname))updateProfile(data);
      if(profile)render();else updateWelcomeRoom();
      for(const {xml,data} of records)if(data.tag!=="HELO"&&data.tag!=="PROF")receive({type:"xml",xml});
      return;
    }
    if (message.type === "error") return error(message.messageKey==="matchUnavailable"?L(1024):message.message, message.resetJoin);
    if(message.type==="nativeNotice"){
      selectedMatches.clear();if(profile)render();
      if(message.kind==="connectionClosed")
        Gb.O("CONNECTION CLOSED<br>"+L(F.Fi),3,()=>send("logout"));
      else if(message.kind==="membershipExpired")
        Gb.O(L(1020)+"(ERR1020)",3,()=>send("subscription"));
      else if(message.kind==="eligibilityRejected")
        Nb.ERR({code:1024});
      return;
    }
    if (message.type === "welcome") {if(returningToLobby&&profile)return render();return renderWelcome(message);}
    if(message.type==="loginState"){loginPending=!!message.pending;if(!profile)kc.Vc(loginPending);return;}
    if(message.type==="returnLobby"){returningToLobby=true;if(profile)render();return;}
    // Recovery stays in the native bridge. Keep only the ordinary lobby UI;
    // a stopped recovery clears any pending match selection without a banner.
    if(message.type==="returnState"){returningToLobby=Number(message.state)!==0;if(Number(message.state)===2){selectedMatches.clear();if(profile)render();}return;}
    // The native table has painted and is covering the lobby. Clear only the
    // saved selection; rebuilding the visible lobby here would flash it idle.
    if(message.type==="gameStarted"){selectedMatches.clear();return;}
    if (message.type === "openPrompt") return R.c5();
    if(message.type==="accountLogs"){
      if(!profile||!accountLogMode||message.sequence!==accountRequestSequence)return;
      const lines=typeof message.response==="string"?message.response.split(/\r?\n/).filter(line=>line.trim()):[];
      const valid=typeof message.response==="string"&&lines.every(line=>{const parts=line.split(" ");return parts.length===10&&/^\d{10}gm-[\da-f]{4}-\d{4}-[\da-f]+$/i.test(parts[0])&&/^[0-3]$/.test(parts[1])&&parts.slice(6).every(score=>/^[+-]?\d+(?:\.\d+)?$/.test(score));});
      accountLogError=!!message.error||!valid;accountResponse=accountLogError?null:(lines.length?lines.join("\n")+"\n":"");renderLogs();return;
    }
    if(message.type==="registerResult"){
      const callback=registration.get(message.sequence);registration.delete(message.sequence);
      if(callback){if(message.error)error(message.error);else callback.call({response:message.response});}return;
    }
    if(message.type==="options"){
      for(const [id,value] of Object.entries(message.values))if(A(id)){if(id==="c53"){E[C.Gb]=value;volumeLabel(value);}else{A(id).checked=!!value;if(id==="c58")C.ob(C.gd,Number(!!value),0);}}
      return;
    }
    if (message.type === "xml") {
      const data=readEvent(message.xml);if(!data)return;
      if ((data.tag === "HELO" || data.tag === "PROF") && (data.PF4 || data.PF3 || data.uname)) {
        updateProfile(data);render();
      } else if (data.tag === "LN") { lastLN = data; if (panel && data.n && data.j && data.g) Z.dh(data); }
      else if (data.tag === "RANKING") { if (panel) Rc.le(data); }
      else if (data.tag === "CHAT") { if (panel) {const oldLobby=S.B;Z.vg(data);if(S.B!==oldLobby){selectedMatches.clear();render();}} }
      else if (data.tag === "CS") { if (panel) Sc.xg(data); }
      else if (data.tag === "DATE") { if (panel) Sc.Ug(data); }
    } else if (message.type === "watch") receiveWatch(message);
    else if (message.type === "logHistory") readLogHistory(message.entries);
    else if (message.type === "logFiles") readLogFiles(message.files).catch(() => error("无法读取本地牌谱。"));
  }
  function storeLogs(logs) {
    for(let i=0;i<40;i++)E.removeItem(C.Ac+i);
    const recent=logs.slice(0,40).reverse();
    recent.forEach((log,index)=>E[C.Ac+index]=JSON.stringify(log));
    E[C.Bc]=recent.length%40;
    if(A("c32"))renderLogs();
  }
  function mergeLogs(){
    const logs=new Map(historyLogs.map(log=>[log.log,log]));
    for(const log of fileLogs){
      const history=logs.get(log.log);
      if(!history){logs.set(log.log,log);continue;}
      let sc=history.sc;
      if(!sc&&log.sc){
        const seat=log.uname.indexOf(history.uname[0]);
        if(seat>=0){const values=log.sc.split(","),rotated=[];for(let at=0;at<values.length;at+=8)for(let i=0;i<4;i++)rotated.push(values[at+2*((seat+i)%4)],values[at+2*((seat+i)%4)+1]);sc=rotated.join(",");}
      }
      logs.set(log.log,{...log,...history,sc});
    }
    storeLogs([...logs.values()].sort((a,b)=>b.log.localeCompare(a.log)).slice(0,40));
  }
  function readLogHistory(entries){
    historyLogs=[];
    for(const entry of entries){
      try{
        const value=Jb(entry),match=(value.file||"").match(/^(\d{10}gm-[\da-f]{4}-\d{4}-[\da-f]+)(?:\.mjlog)?$/i);if(!match)continue;
        const type=Number(value.type??parseInt(match[1].split("-")[1],16)),oya=Number(value.oya||0);
        if(!Number.isInteger(type)||type<0||type>32767||!Number.isInteger(oya)||oya<0||oya>3)continue;
        const sc=/^[-+\d.,]+$/.test(value.sc||"")&&[8,16].includes(value.sc.split(",").length)?value.sc:undefined;
        historyLogs.push({log:match[1],type,oya,lobby:Number(match[1].split("-")[2]),uname:[0,1,2,3].map(i=>value["un"+i]||""),sc});
      }catch(_){/* Ignore one damaged native history entry. */}
    }
    mergeLogs();
  }
  async function readLogFiles(files) {
    const sequence=++fileLogSequence;
    const logs=[];
    for(const file of files) {
      const match=file.name.match(/(\d{10}gm-[\da-f]{4}-\d{4}-[\da-f]+)\.mjlog$/i);
      if(!match)continue;
      try {
        const bytes=Uint8Array.from(atob(file.data),c=>c.charCodeAt(0));
        let content;
        if(bytes[0]===0x1f&&bytes[1]===0x8b){const stream=new Blob([bytes]).stream().pipeThrough(new DecompressionStream("gzip"));content=await new Response(stream).text();}
        else content=new TextDecoder("utf-8",{fatal:true}).decode(bytes);
        const document=new DOMParser().parseFromString(content,"text/xml");if(document.querySelector("parsererror"))continue;
        const users=document.querySelector("UN"),game=document.querySelector("GO"),results=[...document.querySelectorAll("AGARI[owari],RYUUKYOKU[owari]")];
        if(!users||!game||!results.length)continue;
        const scores=results[results.length-1].getAttribute("owari");if(!/^[-+\d.,]+$/.test(scores)||![8,16].includes(scores.split(",").length))continue;
        logs.push({log:match[1],type:Number(game.getAttribute("type")),lobby:Number(game.getAttribute("lobby")||0),oya:0,
          uname:[0,1,2,3].map(seat=>decodeURIComponent(users.getAttribute("n"+seat)||"")),sc:scores});
      }catch(_){/* One damaged record does not hide the other native records. */}
    }
    if(sequence!==fileLogSequence)return;
    fileLogs=logs;mergeLogs();
  }
  if (bridge) bridge.addEventListener("message", event => receive(event.data));
  B.addEventListener("resize", resize); resize();
  renderWelcome();
  send("ready");
})();
