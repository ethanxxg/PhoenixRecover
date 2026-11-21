function qs(sel, root=document){ return root.querySelector(sel); }
function qsa(sel, root=document){ return Array.from(root.querySelectorAll(sel)); }

function setText(id, text){
  const el = document.getElementById(id);
  if (el) el.textContent = text;
}

function go(page){
  window.location.href = page;
}

function setActiveNav(){
  const page = document.body.getAttribute('data-page');
  qsa('.nav a').forEach(a=>{
    const p = a.getAttribute('data-page');
    if (p && p === page) a.classList.add('active');
  });
}

function initLocationPage(){
  const modeEls = qsa('[data-scan-mode]');
  const targetEls = qsa('[data-target]');
  const startBtn = qs('#startScan');

  // P1 已移除扫描模式选择，统一默认 quick
  let mode = 'quick';
  let target = localStorage.getItem('rp.target') || 'disk:C:';

  function apply(){
    localStorage.setItem('rp.scanMode', mode);
    targetEls.forEach(el=>{
      el.classList.toggle('selected', el.getAttribute('data-target') === target);
    });
    if (startBtn) startBtn.classList.remove('disabled');
  }

  // 兼容旧 DOM：如果页面仍然存在 modeEls，也不绑定事件
  targetEls.forEach(el=>{
    el.addEventListener('click', ()=>{
      target = el.getAttribute('data-target');
      localStorage.setItem('rp.target', target);
      apply();
    });
  });

  if (startBtn){
    startBtn.addEventListener('click', ()=>{
      go('p2_scanning.html');
    });
  }

  apply();
}

function initScanningPage(){
  const target = localStorage.getItem('rp.target') || 'disk:C:';
  const targetText = target.startsWith('disk:') ? `本地磁盘(${target.split(':')[1]}:)` :
                     (target === 'recycle' ? '回收站' : target === 'desktop' ? '桌面' : '选择文件夹');

  const stages = [
    { name: '元数据解析', label: '阶段 1/3: FS 元数据解析' },
    { name: '深度扫描',   label: '阶段 2/3: 深度扫描' },
    { name: '签名雕刻',   label: '阶段 3/3: 签名雕刻' },
  ];
  let currentStage = 0;

  setText('scanModeText', stages[0].name);
  setText('scanTargetText', targetText);
  setText('scanStageIndicator', stages[0].label);

  const bar = qs('#progressBar');
  const mFound = qs('#mFound');
  const mScanned = qs('#mScanned');
  const mElapsed = qs('#mElapsed');
  const mSpeed = qs('#mSpeed');
  const mTotal = qs('#mTotal');
  const p2FoundFooter = qs('#p2FoundFooter');
  const fileRows = qs('#fileRows');
  const pathTreeNodes = qs('#pathTreeNodes');
  const typeNodes = qs('#typeNodes');

  // ── Fake streaming scan state (prototype) ───────────────────────────────
  let p = 0;
  let f = 0;
  let start = Date.now();
  let id = 0;
  const files = []; // {id,name,sizeMB,typeCN,typeEN,modified,recovery,path}
  let activeView = 'path'; // 'path'|'type'
  let activePath = 'C:\\';
  let activeType = '图片';
  let activeExt = ''; // when selecting 图片 -> jpg/png/... etc

  const typeList = [
    { cn: '图片', en: '图片', ext: ['jpg','png','jpeg','gif','tif','bmp','ico','webp'] },
    { cn: '视频', en: '视频', ext: ['mp4','mov','avi','mkv','wmv','flv'] },
    { cn: '文档', en: '文档', ext: ['doc','docx','xls','xlsx','ppt','pptx','pdf','txt'] },
    { cn: '音频', en: '音频', ext: ['mp3','wav','flac','aac','m4a','ogg'] },
    { cn: '压缩文件', en: '压缩文件', ext: ['zip','rar','7z','tar','gz'] },
    { cn: '邮件', en: '邮件', ext: ['pst','ost','eml','msg','mbox'] },
    { cn: '书签', en: '书签', ext: ['html','url','webloc'] },
    { cn: '其他', en: '其他', ext: ['dat','bin'] },
    { cn: '未保存的文件', en: '未保存的文件', ext: ['asd','tmp','~'] },
  ];

  const pathBuckets = [
    'C:\\Users\\TH\\Desktop\\',
    'C:\\Users\\TH\\Documents\\',
    'C:\\Users\\TH\\Pictures\\',
    'C:\\Users\\TH\\Videos\\',
    'C:\\Users\\TH\\Music\\',
    'C:\\$Recycle.Bin\\',
    'C:\\ProgramData\\',
    'C:\\Windows\\Temp\\',
  ];

  function rand(arr){ return arr[Math.floor(Math.random()*arr.length)]; }
  function randInt(a,b){ return a + Math.floor(Math.random()*(b-a+1)); }
  function pickRecovery(){
    const r = Math.random();
    if (r < 0.65) return { key:'h', text:'高' };
    if (r < 0.9)  return { key:'m', text:'中' };
    return { key:'l', text:'低' };
  }
  function fmtSize(mb){
    if (mb >= 1024) return (mb/1024).toFixed(1)+' GB';
    if (mb >= 1) return mb.toFixed(1)+' MB';
    return Math.max(1, Math.floor(mb*1024))+' KB';
  }
  function todayMinus(days){
    const d = new Date(Date.now() - days*24*3600*1000);
    const yy = d.getFullYear();
    const mm = String(d.getMonth()+1).padStart(2,'0');
    const dd = String(d.getDate()).padStart(2,'0');
    return `${yy}-${mm}-${dd}`;
  }
  function genName(t){
    const base = rand(['家庭照片','报告','会议记录','发票','视频片段','语音备忘','备份','草稿','书签','邮件导出']);
    const suffix = randInt(1, 9999);
    const ext = rand(t.ext);
    return `${base}_${suffix}.${ext}`;
  }

  function computeCounts(){
    const typeCount = Object.create(null);
    const extCount = Object.create(null); // key: `${typeCN}:${ext}`
    const pathCount = Object.create(null);
    for (const t of typeList) typeCount[t.cn] = 0;
    for (const pb of pathBuckets) pathCount[pb] = 0;

    for (const x of files){
      typeCount[x.typeCN] = (typeCount[x.typeCN]||0) + 1;
      if (x.ext){
        const k = `${x.typeCN}:${x.ext}`;
        extCount[k] = (extCount[k]||0) + 1;
      }
      const bucket = pathBuckets.find(p=>x.path.startsWith(p)) || 'C:\\';
      pathCount[bucket] = (pathCount[bucket]||0) + 1;
    }
    return { typeCount, extCount, pathCount };
  }

  function splitPathParts(fullPath){
    const p = fullPath.replace(/\//g,'\\');
    const idx = p.indexOf('\\');
    if (idx < 0) return ['C:\\'];
    // normalize drive root for demo
    const root = 'C:\\';
    const rest = p.startsWith('C:\\') ? p.substring(3) : p.substring(idx+1);
    const parts = rest.split('\\').filter(Boolean);
    return [root].concat(parts);
  }

  function buildPathTree(){
    // tree node: { name, path, count, children: Map }
    const root = { name:'本地磁盘(C:)', path:'C:\\', count:0, children:new Map() };
    for (const x of files){
      const parts = splitPathParts(x.path);
      let cur = root;
      cur.count++;
      let running = 'C:\\';
      for (let i=1;i<parts.length;i++){
        const seg = parts[i];
        running = running + (running.endsWith('\\')?'':'\\') + seg + '\\';
        if (!cur.children.has(seg)){
          cur.children.set(seg, { name:seg, path:running, count:0, children:new Map() });
        }
        cur = cur.children.get(seg);
        cur.count++;
      }
    }
    return root;
  }

  function renderTypeTree(){
    if (!typeNodes) return;
    const { typeCount, extCount } = computeCounts();

    const openKey = 'rp.typeOpen';
    const open = JSON.parse(localStorage.getItem(openKey) || '{}');
    if (open['图片'] === undefined) open['图片'] = true;

    typeNodes.innerHTML = typeList.map(t=>{
      const total = typeCount[t.cn] || 0;
      const isOpen = !!open[t.cn];
      const caretCls = isOpen ? 'caret open' : 'caret';
      const activeCat = (activeType === t.cn && !activeExt);

      const header = `
        <a class="node ${activeCat?'active':''}" href="#" data-type="${t.cn}">
          <span class="group">
            <span class="${caretCls}" data-caret="${t.cn}">▶</span>
            <span class="checkbox"></span>
            <span>${t.cn}</span>
            <span style="margin-left:auto;color:#9ca3af">${total.toLocaleString()}</span>
          </span>
        </a>`;

      const children = (t.ext || []).map(ext=>{
        const k = `${t.cn}:${ext}`;
        const c = extCount[k] || 0;
        const active = (activeType === t.cn && activeExt === ext);
        return `<a class="node child ${active?'active':''}" href="#" data-type="${t.cn}" data-ext="${ext}" style="${isOpen?'':'display:none'}">
          <span class="group">
            <span class="checkbox"></span>
            <span>${ext}</span>
            <span style="margin-left:auto;color:#9ca3af">${c.toLocaleString()}</span>
          </span>
        </a>`;
      }).join('');

      return header + children;
    }).join('');

    qsa('[data-caret]').forEach(c=>{
      c.addEventListener('click', (e)=>{
        e.preventDefault();
        e.stopPropagation();
        const t = c.getAttribute('data-caret');
        open[t] = !open[t];
        localStorage.setItem(openKey, JSON.stringify(open));
        renderTypeTree();
      });
    });

    qsa('#typeNodes a.node').forEach(a=>{
      a.addEventListener('click', (e)=>{
        e.preventDefault();
        activeType = a.getAttribute('data-type') || '图片';
        activeExt = a.getAttribute('data-ext') || '';
        activeView = 'type';
        renderFiles();
        renderTypeTree();
      });
    });
  }

  function renderPathTree(){
    if (!pathTreeNodes) return;

    const openKey = 'rp.pathOpen';
    const open = JSON.parse(localStorage.getItem(openKey) || '{}');
    if (open['C:\\'] === undefined) open['C:\\'] = true;

    const tree = buildPathTree();

    function esc(s){ return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;'); }

    function renderNode(node, depth){
      const isOpen = !!open[node.path];
      const caretCls = node.children.size ? (isOpen ? 'caret open' : 'caret') : 'caret';
      const caret = node.children.size ? `<span class="${caretCls}" data-pcaret="${esc(node.path)}">▶</span>` : `<span class="caret"></span>`;
      const active = activePath === node.path;
      const label = depth === 0 ? node.name : node.name;
      let html = `
        <a class="node ${active?'active':''}" href="#" data-path="${esc(node.path)}" style="padding-left:${8 + depth*14}px">
          <span class="group">
            ${caret}
            <span class="checkbox"></span>
            <span>${esc(label)}</span>
            <span style="margin-left:auto;color:#9ca3af">${node.count.toLocaleString()}</span>
          </span>
        </a>`;
      if (node.children.size && isOpen){
        const children = Array.from(node.children.values())
          .sort((a,b)=>a.name.localeCompare(b.name))
          .slice(0, 30); // cap for prototype
        for (const ch of children){
          html += renderNode(ch, depth+1);
        }
      }
      return html;
    }

    pathTreeNodes.innerHTML = renderNode(tree, 0);

    qsa('[data-pcaret]').forEach(c=>{
      c.addEventListener('click', (e)=>{
        e.preventDefault();
        e.stopPropagation();
        const p = c.getAttribute('data-pcaret');
        open[p] = !open[p];
        localStorage.setItem(openKey, JSON.stringify(open));
        renderPathTree();
      });
    });

    qsa('#pathTreeNodes a.node').forEach(a=>{
      a.addEventListener('click', (e)=>{
        e.preventDefault();
        activePath = a.getAttribute('data-path') || 'C:\\';
        activeView = 'path';
        renderFiles();
        renderPathTree();
      });
    });
  }

  function refillTable(){
    if (!fileRows) return;
    const shown = files.filter(x=>{
      if (activeView === 'type') {
        if (x.typeCN !== activeType) return false;
        if (activeExt) return x.ext === activeExt;
        return true;
      }
      return x.path.startsWith(activePath);
    }).slice(-200); // keep latest 200 for prototype performance

    fileRows.innerHTML = shown.map(x=>{
      return `<tr>
        <td><input type="checkbox" /></td>
        <td>${x.name}</td>
        <td>${x.size}</td>
        <td>${x.typeCN}</td>
        <td>${x.modified}</td>
        <td><span class="tag ${x.recovery.key}">${x.recovery.text}</span></td>
      </tr>`;
    }).join('');
  }

  // 给全局 Tab 切换提供回调：切换后刷新表格过滤
  window.__rpSetActiveView = (kind)=>{
    activeView = (kind === 'type') ? 'type' : 'path';
    refillTable();
  };

  function appendBatch(batch){
    for (const x of batch){
      files.push(x);
    }
    f = files.length;
    try {
      localStorage.setItem('rp.lastFiles', JSON.stringify(files.slice(-1500)));
    } catch {}
    renderPathTree();
    renderTypeTree();
    refillTable();
  }

  // Build initial trees
  renderPathTree();
  renderTypeTree();
  refillTable();

  const timer = setInterval(()=>{
    p = Math.min(100, p + Math.random()*6);
    // Stream in a batch of files
    const batchSize = randInt(12, 36);
    const batch = [];
    for (let i=0;i<batchSize;i++){
      const t = rand(typeList);
      const rec = pickRecovery();
      const sizeMb = Math.random() < 0.2 ? randInt(1, 20) : (Math.random()*8+0.05);
      const path = rand(pathBuckets);
      batch.push({
        id: ++id,
        typeCN: t.cn,
        typeEN: t.en,
        name: genName(t),
        ext: '',
        size: fmtSize(sizeMb),
        modified: todayMinus(randInt(0, 400)),
        recovery: rec,
        path
      });
    }
    for (const x of batch){
      const m = /\.([a-zA-Z0-9~]+)$/.exec(x.name);
      x.ext = m ? m[1].toLowerCase() : '';
    }
    appendBatch(batch);
    const sec = Math.floor((Date.now()-start)/1000);
    const mm = String(Math.floor(sec/60)).padStart(2,'0');
    const ss = String(sec%60).padStart(2,'0');

    const newStage = p < 33 ? 0 : (p < 70 ? 1 : 2);
    if (newStage !== currentStage) {
      currentStage = newStage;
      setText('scanModeText', stages[currentStage].name);
      setText('scanStageIndicator', stages[currentStage].label);
    }

    if (bar) bar.style.width = `${p}%`;
    const scannedGb = (p*2.56).toFixed(0);
    if (mFound) mFound.textContent = f.toLocaleString();
    if (p2FoundFooter) p2FoundFooter.textContent = f.toLocaleString();
    if (mScanned) mScanned.textContent = `${scannedGb} GB`;
    if (mTotal && !mTotal.textContent) mTotal.textContent = '500 GB';
    if (mElapsed) mElapsed.textContent = `${mm}:${ss}`;
    if (mSpeed) {
      const speed = Math.floor(120 + Math.random()*80);
      mSpeed.textContent = `${speed} MB/s`;
    }

    if (p >= 100){
      clearInterval(timer);
      setTimeout(()=>go('p3_results.html'), 450);
    }
  }, 450);

  const stopBtn = qs('#stopScan');
  if (stopBtn){
    stopBtn.addEventListener('click', ()=>{
      clearInterval(timer);
      localStorage.setItem('rp.scanCancelled', '1');
      try {
        localStorage.setItem('rp.lastFiles', JSON.stringify(files.slice(-1500)));
      } catch {}
      go('p3_results.html');
    });
  }

  // 保留：可扩展其它控制逻辑
}

function initResultsPage(){
  const cancelled = localStorage.getItem('rp.scanCancelled') === '1';
  const mode = localStorage.getItem('rp.scanMode') || 'quick';
  const target = localStorage.getItem('rp.target') || 'disk:C:';

  const modeText = mode === 'quick' ? '快速扫描' : (mode === 'deep' ? '深度扫描' : 'RAW 扫描');
  const targetText = target.startsWith('disk:') ? `本地磁盘(${target.split(':')[1]}:)` :
                     (target === 'recycle' ? '回收站' : target === 'desktop' ? '桌面' : '选择文件夹');
  // P3 顶部文案可能被精简：仅在元素存在时设置
  if (qs('#resultModeText')) setText('resultModeText', modeText);
  if (qs('#resultTargetText')) setText('resultTargetText', targetText);
  if (qs('#resultState')) setText('resultState', cancelled ? '扫描已停止（保留已发现结果）' : '扫描完成');

  // 结果页：左侧保持与扫描页一致（Path/Type + 可展开树），右侧使用缩略图网格
  const pathTreeNodes = qs('#pathTreeNodes');
  const typeNodes = qs('#typeNodes');
  const thumbGrid = qs('#thumbGrid');
  const selectedCount = qs('#selectedCount');
  const foundCountText = qs('#foundCountText');
  const gridTitle = qs('#gridTitle');
  const previewPanel = qs('#previewPanel');
  const listView = qs('#listView');
  const viewToggle = qs('#viewToggle');
  const searchInput = qs('#searchInput');
  const searchBtn = qs('#searchBtn');
  const backBtn = qs('#backToLocation');

  const raw = localStorage.getItem('rp.lastFiles');
  let files = [];
  if (raw) {
    try {
      const list = JSON.parse(raw);
      if (Array.isArray(list)) files = list;
    } catch {}
  }

  let activeView = 'path'; // 'path'|'type'
  let activePath = 'C:\\';
  let activeType = '图片';
  let activeExt = '';
  const selectedIds = new Set();
  let viewMode = localStorage.getItem('rp.p3.viewMode') || 'grid'; // 'grid'|'list'
  let searchQuery = '';

  const typeList = [
    { cn: '图片', en: '图片', ext: ['jpg','png','jpeg','gif','tif','bmp','ico','webp'] },
    { cn: '视频', en: '视频', ext: ['mp4','mov','avi','mkv','wmv','flv'] },
    { cn: '文档', en: '文档', ext: ['doc','docx','xls','xlsx','ppt','pptx','pdf','txt'] },
    { cn: '音频', en: '音频', ext: ['mp3','wav','flac','aac','m4a','ogg'] },
    { cn: '压缩文件', en: '压缩文件', ext: ['zip','rar','7z','tar','gz'] },
    { cn: '邮件', en: '邮件', ext: ['pst','ost','eml','msg','mbox'] },
    { cn: '书签', en: '书签', ext: ['html','url','webloc'] },
    { cn: '其他', en: '其他', ext: ['dat','bin'] },
    { cn: '未保存的文件', en: '未保存的文件', ext: ['asd','tmp','~'] },
  ];

  const pathBuckets = [
    'C:\\Users\\TH\\Desktop\\',
    'C:\\Users\\TH\\Documents\\',
    'C:\\Users\\TH\\Pictures\\',
    'C:\\Users\\TH\\Videos\\',
    'C:\\Users\\TH\\Music\\',
    'C:\\$Recycle.Bin\\',
    'C:\\ProgramData\\',
    'C:\\Windows\\Temp\\',
  ];

  function computeCounts(){
    const typeCount = Object.create(null);
    const extCount = Object.create(null); // key: `${typeCN}:${ext}`
    for (const t of typeList) typeCount[t.cn] = 0;
    for (const x of files){
      typeCount[x.typeCN] = (typeCount[x.typeCN]||0) + 1;
      if (x.ext){
        const k = `${x.typeCN}:${x.ext}`;
        extCount[k] = (extCount[k]||0) + 1;
      }
    }
    return { typeCount, extCount };
  }

  function splitPathParts(fullPath){
    const p = String(fullPath || '').replace(/\//g,'\\');
    const idx = p.indexOf('\\');
    if (idx < 0) return ['C:\\'];
    const root = 'C:\\';
    const rest = p.startsWith('C:\\') ? p.substring(3) : p.substring(idx+1);
    const parts = rest.split('\\').filter(Boolean);
    return [root].concat(parts);
  }

  function buildPathTree(){
    const root = { name:'本地磁盘(C:)', path:'C:\\', count:0, children:new Map() };
    for (const x of files){
      const parts = splitPathParts(x.path);
      let cur = root;
      cur.count++;
      for (let i=1;i<parts.length;i++){
        const seg = parts[i];
        let running = 'C:\\';
        for (let k=1;k<=i;k++){
          running += parts[k] + '\\';
        }
        if (!cur.children.has(seg)){
          cur.children.set(seg, { name:seg, path:running, count:0, children:new Map() });
        }
        cur = cur.children.get(seg);
        cur.count++;
      }
    }
    return root;
  }

  function renderTypeTree(){
    if (!typeNodes) return;
    const { typeCount, extCount } = computeCounts();

    const openKey = 'rp.typeOpen';
    const open = JSON.parse(localStorage.getItem(openKey) || '{}');
    if (open['图片'] === undefined) open['图片'] = true;

    typeNodes.innerHTML = typeList.map(t=>{
      const total = typeCount[t.cn] || 0;
      const isOpen = !!open[t.cn];
      const caretCls = isOpen ? 'caret open' : 'caret';
      const activeCat = (activeType === t.cn && !activeExt);

      const header = `
        <a class="node ${activeCat?'active':''}" href="#" data-type="${t.cn}">
          <span class="group">
            <span class="${caretCls}" data-caret="${t.cn}">▶</span>
            <span class="checkbox"></span>
            <span>${t.cn}</span>
            <span style="margin-left:auto;color:#9ca3af">${total.toLocaleString()}</span>
          </span>
        </a>`;

      const children = (t.ext || []).map(ext=>{
        const k = `${t.cn}:${ext}`;
        const c = extCount[k] || 0;
        const active = (activeType === t.cn && activeExt === ext);
        return `<a class="node child ${active?'active':''}" href="#" data-type="${t.cn}" data-ext="${ext}" style="${isOpen?'':'display:none'}">
          <span class="group">
            <span class="checkbox"></span>
            <span>${ext}</span>
            <span style="margin-left:auto;color:#9ca3af">${c.toLocaleString()}</span>
          </span>
        </a>`;
      }).join('');

      return header + children;
    }).join('');

    qsa('[data-caret]').forEach(c=>{
      c.addEventListener('click', (e)=>{
        e.preventDefault();
        e.stopPropagation();
        const t = c.getAttribute('data-caret');
        open[t] = !open[t];
        localStorage.setItem(openKey, JSON.stringify(open));
        renderTypeTree();
      });
    });

    qsa('#typeNodes a.node').forEach(a=>{
      a.addEventListener('click', (e)=>{
        e.preventDefault();
        activeType = a.getAttribute('data-type') || '图片';
        activeExt = a.getAttribute('data-ext') || '';
        activeView = 'type';
        renderFiles();
        renderTypeTree();
      });
    });
  }

  function renderPathTree(){
    if (!pathTreeNodes) return;

    const openKey = 'rp.pathOpen';
    const open = JSON.parse(localStorage.getItem(openKey) || '{}');
    if (open['C:\\'] === undefined) open['C:\\'] = true;

    const tree = buildPathTree();

    function esc(s){ return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;'); }

    function renderNode(node, depth){
      const isOpen = !!open[node.path];
      const caretCls = node.children.size ? (isOpen ? 'caret open' : 'caret') : 'caret';
      const caret = node.children.size ? `<span class="${caretCls}" data-pcaret="${esc(node.path)}">▶</span>` : `<span class="caret"></span>`;
      const active = activePath === node.path;
      let html = `
        <a class="node ${active?'active':''}" href="#" data-path="${esc(node.path)}" style="padding-left:${8 + depth*14}px">
          <span class="group">
            ${caret}
            <span class="checkbox"></span>
            <span>${esc(depth===0?node.name:node.name)}</span>
            <span style="margin-left:auto;color:#9ca3af">${(node.count||0).toLocaleString()}</span>
          </span>
        </a>`;
      if (node.children.size && isOpen){
        const children = Array.from(node.children.values())
          .sort((a,b)=>a.name.localeCompare(b.name))
          .slice(0, 30);
        for (const ch of children){
          html += renderNode(ch, depth+1);
        }
      }
      return html;
    }

    pathTreeNodes.innerHTML = renderNode(tree, 0);

    qsa('[data-pcaret]').forEach(c=>{
      c.addEventListener('click', (e)=>{
        e.preventDefault();
        e.stopPropagation();
        const p = c.getAttribute('data-pcaret');
        open[p] = !open[p];
        localStorage.setItem(openKey, JSON.stringify(open));
        renderPathTree();
      });
    });

    qsa('#pathTreeNodes a.node').forEach(a=>{
      a.addEventListener('click', (e)=>{
        e.preventDefault();
        activePath = a.getAttribute('data-path') || 'C:\\';
        activeView = 'path';
        renderFiles();
        renderPathTree();
      });
    });
  }

  function fileMatchesActive(x){
    if (activeView === 'type') {
      if (x.typeCN !== activeType) return false;
      if (activeExt) return x.ext === activeExt;
      return true;
    }
    return String(x.path || '').startsWith(activePath);
  }

  function shownFiles(){
    const q = String(searchQuery || '').trim().toLowerCase();
    if (!q) return files.filter(fileMatchesActive);
    return files.filter(x=>{
      if (!fileMatchesActive(x)) return false;
      const name = String(x.name||'').toLowerCase();
      const path = String(x.path||'').toLowerCase();
      return name.includes(q) || path.includes(q);
    });
  }

  function syncViewModeUI(){
    if (thumbGrid) thumbGrid.classList.toggle('hidden', viewMode !== 'grid');
    if (listView) listView.classList.toggle('hidden', viewMode !== 'list');
    if (viewToggle) viewToggle.textContent = (viewMode === 'grid') ? '列表' : '预览';
  }

  function renderFiles(){
    if (viewMode === 'list') renderList();
    else renderGrid();
  }

  function renderGrid(){
    if (!thumbGrid) return;
    const shown = shownFiles();
    const list = shown
      .slice()
      .sort((a,b)=>String(a.path||'').localeCompare(String(b.path||'')) || String(a.name||'').localeCompare(String(b.name||'')))
      .slice(0, 240); // stable + obvious changes per filter

    if (gridTitle){
      if (activeView === 'type'){
        gridTitle.textContent = activeExt ? `${activeType} / .${activeExt}` : activeType;
      } else {
        gridTitle.textContent = activePath === 'C:\\' ? '本地磁盘(C:)' : activePath;
      }
    }
    if (foundCountText) foundCountText.textContent = shown.length.toLocaleString();
    if (selectedCount) selectedCount.textContent = String(selectedIds.size);

    thumbGrid.innerHTML = list.map(x=>{
      const sid = String(x.id || x.name || x.path);
      const sel = selectedIds.has(sid);
      const name = x.name || '';
      const subL = x.size || '';
      const subR = x.modified || '';
      const rec = x.recovery || { key:'h', text:'高' };
      const recColor = rec.key === 'h' ? 'var(--success)' : (rec.key === 'm' ? 'var(--warning)' : 'var(--danger)');
      return `<div class="thumb-card ${sel?'selected':''}" data-sid="${sid}">
        <div class="thumb-check">${sel?'✓':''}</div>
        <div class="thumb-img"></div>
        <div class="thumb-name" title="${name.replace(/"/g,'&quot;')}">${name}</div>
        <div class="thumb-sub"><span>${subL}</span><span style="color:${recColor};font-weight:700">${rec.text}</span><span>${subR}</span></div>
      </div>`;
    }).join('');

    qsa('#thumbGrid .thumb-card').forEach(card=>{
      card.addEventListener('click', ()=>{
        const sid = card.getAttribute('data-sid');
        if (!sid) return;
        if (selectedIds.has(sid)) selectedIds.delete(sid); else selectedIds.add(sid);
        if (selectedCount) selectedCount.textContent = String(selectedIds.size);
        card.classList.toggle('selected', selectedIds.has(sid));
        const c = card.querySelector('.thumb-check');
        if (c) c.textContent = selectedIds.has(sid) ? '✓' : '';
      });
      card.addEventListener('dblclick', ()=>{
        const sid = card.getAttribute('data-sid');
        const x = files.find(f => String(f.id || f.name || f.path) === String(sid));
        if (!previewPanel || !x) return;
        previewPanel.innerHTML = `<div style="padding:14px">
          <div style="font-weight:900;margin-bottom:6px">${x.name||''}</div>
          <div class="note">${(x.path||'')}${(x.ext?('  .'+x.ext):'')}</div>
          <div class="note" style="margin-top:8px">预览：原型占位</div>
        </div>`;
      });
    });
    // reset scroll so user sees updated list
    try { thumbGrid.scrollTop = 0; } catch {}
  }

  function renderList(){
    if (!listView) return;
    const shown = shownFiles();
    const list = shown
      .slice()
      .sort((a,b)=>String(a.path||'').localeCompare(String(b.path||'')) || String(a.name||'').localeCompare(String(b.name||'')))
      .slice(0, 240);
    if (gridTitle){
      if (activeView === 'type'){
        gridTitle.textContent = activeExt ? `${activeType} / .${activeExt}` : activeType;
      } else {
        gridTitle.textContent = activePath === 'C:\\' ? '本地磁盘(C:)' : activePath;
      }
    }
    if (foundCountText) foundCountText.textContent = shown.length.toLocaleString();
    if (selectedCount) selectedCount.textContent = String(selectedIds.size);

    listView.innerHTML = list.map(x=>{
      const sid = String(x.id || x.name || x.path);
      const sel = selectedIds.has(sid);
      const name = (x.name || '').replace(/"/g,'&quot;');
      const meta1 = x.size || '';
      const meta2 = x.modified || '';
      const meta3 = x.ext ? ('.'+x.ext) : (x.typeCN || '');
      const rec = x.recovery || { key:'h', text:'高' };
      const recColor = rec.key === 'h' ? 'var(--success)' : (rec.key === 'm' ? 'var(--warning)' : 'var(--danger)');
      return `<div class="list-row ${sel?'selected':''}" data-sid="${sid}">
        <div class="list-check">${sel?'✓':''}</div>
        <div class="list-name" title="${name}">${x.name||''}</div>
        <div class="list-meta"><span>${meta1}</span><span>${meta3}</span><span style="color:${recColor};font-weight:700">${rec.text}</span><span>${meta2}</span></div>
      </div>`;
    }).join('');

    qsa('#listView .list-row').forEach(row=>{
      row.addEventListener('click', ()=>{
        const sid = row.getAttribute('data-sid');
        if (!sid) return;
        if (selectedIds.has(sid)) selectedIds.delete(sid); else selectedIds.add(sid);
        if (selectedCount) selectedCount.textContent = String(selectedIds.size);
        row.classList.toggle('selected', selectedIds.has(sid));
        const c = row.querySelector('.list-check');
        if (c) c.textContent = selectedIds.has(sid) ? '✓' : '';
      });
      row.addEventListener('dblclick', ()=>{
        const sid = row.getAttribute('data-sid');
        const x = files.find(f => String(f.id || f.name || f.path) === String(sid));
        if (!previewPanel || !x) return;
        previewPanel.innerHTML = `<div style="padding:14px">
          <div style="font-weight:900;margin-bottom:6px">${x.name||''}</div>
          <div class="note">${(x.path||'')}${(x.ext?('  .'+x.ext):'')}</div>
          <div class="note" style="margin-top:8px">预览：原型占位</div>
        </div>`;
      });
    });
    try { listView.scrollTop = 0; } catch {}
  }

  // 让全局 Tab 切换时也能更新中间表格过滤
  window.__rpSetActiveView = (kind)=>{
    activeView = (kind === 'type') ? 'type' : 'path';
    renderFiles();
  };

  renderPathTree();
  renderTypeTree();
  syncViewModeUI();
  renderFiles();

  if (backBtn){
    backBtn.addEventListener('click', ()=>go('p1_location.html'));
  }
  if (viewToggle){
    viewToggle.addEventListener('click', ()=>{
      viewMode = (viewMode === 'grid') ? 'list' : 'grid';
      try { localStorage.setItem('rp.p3.viewMode', viewMode); } catch {}
      syncViewModeUI();
      renderFiles();
    });
  }
  function applySearch(){
    searchQuery = searchInput ? String(searchInput.value || '') : '';
    renderFiles();
  }
  if (searchBtn) searchBtn.addEventListener('click', applySearch);
  if (searchInput){
    searchInput.addEventListener('keydown', (e)=>{
      if (e.key === 'Enter') applySearch();
    });
    searchInput.addEventListener('input', ()=>{
      // 轻量实时过滤
      searchQuery = String(searchInput.value || '');
      renderFiles();
    });
  }

  const recoverBtn = qs('#recoverSelected');
  if (recoverBtn){
    recoverBtn.addEventListener('click', ()=>go('p4_target.html'));
  }
  const recoverAllBtn = qs('#recoverAllBtn');
  if (recoverAllBtn){
    recoverAllBtn.addEventListener('click', ()=>go('p4_target.html'));
  }
  const newBtn = qs('#newScan');
  if (newBtn){
    newBtn.addEventListener('click', ()=>{
      localStorage.removeItem('rp.scanCancelled');
      go('p1_location.html');
    });
  }
}

function initTargetPage(){
  const back = qs('#backToResults');
  if (back) back.addEventListener('click', ()=>go('p3_results.html'));
  const start = qs('#startRecover');
  if (start) start.addEventListener('click', ()=>go('p5_complete.html'));
}

function initCompletePage(){
  const newBtn = qs('#newRecovery');
  if (newBtn) newBtn.addEventListener('click', ()=>{
    localStorage.removeItem('rp.scanCancelled');
    go('p1_location.html');
  });
}

function initRepairPage(){
  const dropZone = qs('#repairDropZone');
  const fileInput = qs('#repairFileInput');
  const fileList = qs('#repairFileList');
  const analyzeBtn = qs('#repairAnalyze');
  const step1 = qs('#repairStep1');
  const step2 = qs('#repairStep2');
  const step3 = qs('#repairStep3');
  const backBtn = qs('#repairBack');
  const progressBar = qs('#repairProgressBar');
  const progressText = qs('#repairProgressText');

  const demoFiles = [
    { name:'damaged_photo.jpg', type:'JPEG 图片', damage:'中度损坏', damageColor:'var(--warning)' },
    { name:'archive.zip',       type:'ZIP 压缩包', damage:'严重损坏', damageColor:'var(--danger)' },
    { name:'video_clip.mp4',    type:'MP4 视频',  damage:'轻微损坏', damageColor:'var(--success)' },
    { name:'screenshot.png',    type:'PNG 图片',  damage:'中度损坏', damageColor:'var(--warning)' },
  ];

  if (dropZone){
    dropZone.addEventListener('click', ()=>{ if(fileInput) fileInput.click(); });
    dropZone.addEventListener('dragover', (e)=>{ e.preventDefault(); dropZone.style.borderColor='var(--primary2)'; });
    dropZone.addEventListener('dragleave', ()=>{ dropZone.style.borderColor='#cfd6df'; });
    dropZone.addEventListener('drop', (e)=>{ e.preventDefault(); dropZone.style.borderColor='#cfd6df'; showDemoFiles(); });
  }
  if (fileInput){
    fileInput.addEventListener('change', showDemoFiles);
  }

  function showDemoFiles(){
    if (!fileList) return;
    fileList.innerHTML = demoFiles.map(f=>`<div style="display:flex;align-items:center;gap:10px;padding:8px 0;border-bottom:1px solid var(--border)">
      <span style="font-weight:700">${f.name}</span>
      <span class="note" style="margin:0">${f.type}</span>
    </div>`).join('');
  }

  if (analyzeBtn){
    analyzeBtn.addEventListener('click', ()=>{
      if (step1) step1.classList.add('hidden');
      if (step2) step2.classList.remove('hidden');
      setText('repairFileName', demoFiles[0].name);
      setText('repairFileType', demoFiles[0].type);
      const dmg = qs('#repairDamageLevel');
      if (dmg){ dmg.textContent = demoFiles[0].damage; dmg.style.color = demoFiles[0].damageColor; }

      let p = 0;
      const timer = setInterval(()=>{
        p = Math.min(100, p + Math.random()*12);
        if (progressBar) progressBar.style.width = `${p}%`;
        if (progressText) progressText.textContent = p < 100 ? `正在修复... ${Math.floor(p)}%` : '修复完成';
        if (p >= 100){
          clearInterval(timer);
          setTimeout(()=>{
            if (step2) step2.classList.add('hidden');
            if (step3) step3.classList.remove('hidden');
          }, 400);
        }
      }, 200);
    });
  }

  if (backBtn){
    backBtn.addEventListener('click', ()=>{
      if (step3) step3.classList.add('hidden');
      if (step1) step1.classList.remove('hidden');
    });
  }
}

function initImagingPage(){
  const srcCards = qsa('[data-img-src]');
  const startBtn = qs('#imgStart');
  const progressSection = qs('#imgProgress');
  const completeSection = qs('#imgComplete');
  const errorSection = qs('#imgError');
  const showErrorBtn = qs('#imgShowError');
  const progressBar = qs('#imgProgressBar');

  let selectedSrc = 'disk0';

  srcCards.forEach(card=>{
    card.addEventListener('click', ()=>{
      srcCards.forEach(c=>c.classList.remove('selected'));
      card.classList.add('selected');
      selectedSrc = card.getAttribute('data-img-src');
    });
  });

  if (startBtn){
    startBtn.addEventListener('click', ()=>{
      if (progressSection) progressSection.classList.remove('hidden');
      if (completeSection) completeSection.classList.add('hidden');
      if (errorSection) errorSection.classList.add('hidden');
      startBtn.disabled = true;

      let p = 0;
      const timer = setInterval(()=>{
        p = Math.min(100, p + Math.random()*5);
        if (progressBar) progressBar.style.width = `${p}%`;
        setText('imgPercent', `${Math.floor(p)}%`);
        setText('imgCopied', `${(p*4.658).toFixed(1)} GB`);
        setText('imgSpeed', `${Math.floor(80 + Math.random()*60)} MB/s`);
        const remain = p < 100 ? `${Math.max(1, Math.floor((100-p)*0.8))} 分钟` : '完成';
        setText('imgETA', remain);

        if (p >= 100){
          clearInterval(timer);
          setTimeout(()=>{
            if (completeSection) completeSection.classList.remove('hidden');
            startBtn.disabled = false;
          }, 400);
        }
      }, 300);
    });
  }

  if (showErrorBtn){
    showErrorBtn.addEventListener('click', ()=>{
      if (progressSection) progressSection.classList.add('hidden');
      if (completeSection) completeSection.classList.add('hidden');
      if (errorSection) errorSection.classList.toggle('hidden');
    });
  }
}

function initSmartPage(){
  const diskCards = qsa('[data-smart-disk]');
  const tableBody = qs('#smartTableBody');
  const diskNameEl = qs('#smartDiskName');

  const diskData = [
    {
      name: 'Samsung 970 EVO Plus (C:)',
      attrs: [
        ['05','重映射扇区计数','100','10','0','h','良好'],
        ['C5','当前待映射扇区','100','0','0','h','良好'],
        ['C6','无法修正的扇区','100','0','0','h','良好'],
        ['C2','温度','62','0','38°C','h','良好'],
        ['09','通电时间','88','0','12,456 h','h','良好'],
        ['F1','总写入量 (TBW)','75','0','68.2 TB','h','良好'],
        ['B1','磨损均衡计数','92','0','8%','h','良好'],
      ]
    },
    {
      name: 'WD Blue 1TB (D:)',
      attrs: [
        ['05','重映射扇区计数','85','10','24','m','警告'],
        ['C5','当前待映射扇区','90','0','8','m','警告'],
        ['C6','无法修正的扇区','95','0','3','h','良好'],
        ['C2','温度','45','0','45°C','m','警告'],
        ['09','通电时间','62','0','38,920 h','h','良好'],
        ['0A','启停周期','78','0','12,340','h','良好'],
        ['BB','可修正错误','88','0','156','m','警告'],
      ]
    },
    {
      name: 'Seagate Portable (E:)',
      attrs: [
        ['05','重映射扇区计数','30','10','312','l','危险'],
        ['C5','当前待映射扇区','25','0','89','l','危险'],
        ['C6','无法修正的扇区','40','0','47','l','危险'],
        ['C2','温度','38','0','52°C','l','危险'],
        ['09','通电时间','31','0','56,100 h','m','警告'],
        ['0A','启停周期','42','0','45,600','m','警告'],
        ['BB','可修正错误','20','0','2,847','l','危险'],
      ]
    }
  ];

  function renderTable(idx){
    const d = diskData[idx];
    if (diskNameEl) diskNameEl.textContent = d.name;
    if (!tableBody) return;
    tableBody.innerHTML = d.attrs.map(a=>`<tr>
      <td>${a[0]}</td><td>${a[1]}</td><td>${a[2]}</td><td>${a[3]}</td><td>${a[4]}</td>
      <td><span class="tag ${a[5]}">${a[6]}</span></td>
    </tr>`).join('');
  }

  diskCards.forEach(card=>{
    card.addEventListener('click', ()=>{
      diskCards.forEach(c=>c.classList.remove('selected'));
      card.classList.add('selected');
      renderTable(parseInt(card.getAttribute('data-smart-disk')) || 0);
    });
  });

  const refreshBtn = qs('#smartRefresh');
  if (refreshBtn){
    refreshBtn.addEventListener('click', ()=>{
      alert('原型占位：刷新 S.M.A.R.T. 数据');
    });
  }
}

document.addEventListener('DOMContentLoaded', ()=>{
  setActiveNav();
  const page = document.body.getAttribute('data-page');
  if (page === 'p1') initLocationPage();
  if (page === 'p2') initScanningPage();
  if (page === 'p3') initResultsPage();
  if (page === 'p4') initTargetPage();
  if (page === 'p5') initCompletePage();
  if (page === 'p6') initRepairPage();
  if (page === 'p7') initImagingPage();
  if (page === 'p8') initSmartPage();

  // Simple tab switcher for Path/Type view (prototype only)
  const tabs = qsa('.view-tabs a[data-tab]');
  if (tabs.length) {
    const treePath = qs('#treePath');
    const treeType = qs('#treeType');
    tabs.forEach(t => {
      t.addEventListener('click', () => {
        tabs.forEach(x => x.classList.remove('active'));
        t.classList.add('active');
        const kind = t.getAttribute('data-tab');
        if (treePath && treeType) {
          treePath.classList.toggle('hidden', kind !== 'path');
          treeType.classList.toggle('hidden', kind !== 'type');
        }
        if (window.__rpSetActiveView) {
          window.__rpSetActiveView(kind === 'type' ? 'type' : 'path');
        }
      });
    });
  }
});

