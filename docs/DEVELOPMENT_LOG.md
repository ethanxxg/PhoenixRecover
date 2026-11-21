## LOG-20260527-015

| 字段 | 内容 |
|------|------|
| 日期 | 2026-05-27 |
| 类型 | 版本号统一 |
| 版本 | **1.0.0.20260527** |

### 变更摘要

1. 新增根目录 `VERSION` 为唯一版本源；`scripts/Get-RepoVersion.ps1` 供发布脚本读取。
2. CMake 生成 `PhoenixRecover.rc`（产品/文件版本字符串）；主窗口标题显示版本。
3. `Build-Release.ps1` / `Create-Installer.ps1` / `Release-Windows.ps1` 默认从 `VERSION` 取号；安装包名 `PhoenixRecover-Setup-1.0.0.20260527.exe`。

---

## LOG-20260527-014

| 字段 | 内容 |
|------|------|
| 日期 | 2026-05-27 |
| 类型 | 商业化 P0：修复 UI + 发布门禁 + 合规 |
| 关联里程碑 | P0-3 ~ P0-6 |

### 变更摘要

1. 新增 `FileRepairPage`、`FileRepairController/Worker`，侧栏「文件修复」入口；支持批量修复、进度/取消、CSV/TXT 报告导出。
2. `tests/test_repair_samples.cpp`（需 Qt Core + `testdata/repair_samples`）；`CrashDiagnostics` 已纳入 CTest。
3. 新增 `scripts/PreRelease.ps1`；`docs/THIRD_PARTY_NOTICE.md`、`docs/SBOM.template.json`。
4. CMake 选项 `PHOENIX_HAVE_LIBZIP`：开启后 ZipRepair 用 libzip 做归档校验。

### 验证

- `cmake -DPHOENIX_BUILD_GUI=OFF` + `ctest`：**12/12 通过**（含 CrashDiagnostics）。

---

## LOG-20260527-013

| 字段 | 内容 |
|------|------|
| 日期 | 2026-05-27 |
| 类型 | 商业化 P0：发布闭环 + 崩溃诊断 |
| 关联里程碑 | P0-1, P0-2 |

### 变更摘要

1. `scripts/Build-Release.ps1` 增强：支持 `-Installer`（NSIS/WiX/None）、`-RunSmokeTest`、`-SignCommand`。
2. 新增 `installer/PhoenixRecover.nsi`（NSIS 安装模板）。
3. 新增 `scripts/Create-Installer.ps1` 与 `scripts/Smoke-Installer.ps1`。
4. 新增 `crash/CrashHandler.h/.cpp`，接入 `main.cpp`，实现未捕获异常时 dmp + log 副本归档。
5. 新增 `tests/test_crash_diagnostics.cpp` 与 CTest `CrashDiagnostics`。

---
# PhoenixRecover 寮€鍙戝彉鏇翠笌淇璁板綍

> **鐢ㄩ€?*: 鎸夋椂闂村€掑簭璁板綍姣忔瀹炵幇銆侀噸鏋勩€佺己闄蜂慨澶嶅強楠岃瘉鎯呭喌锛屼笌 [TECHNICAL_PLAN.md](./TECHNICAL_PLAN.md) 閲岀▼纰戝搴旀椂鍙氦鍙夊紩鐢ㄣ€? 
> **绾﹀畾**: 鏂拌褰曡拷鍔犲湪**鏂囬**锛堟渶鏂板湪涓婏級锛涙潯鐩娇鐢ㄧ粺涓€缂栧彿 `LOG-YYYYMMDD-搴忓彿`銆?

---

## LOG-20260526-007

| 瀛楁 | 鍐呭 |
|------|------|
| 鏃ユ湡 | 2026-05-26 |
| 绫诲瀷 | 鍙戝竷鍑嗗锛堟枃妗ｄ綋绯?+ 鏋勫缓鑴氭湰 + 璁稿彲璇侊級 |
| 鍏宠仈閲岀▼纰?| RELEASE_CHECKLIST |

### 鍙樻洿鎽樿

1. **鏂板缓 `docs/CHANGELOG.md`**
   - 鍩轰簬 DEVELOPMENT_LOG.md 鍏ㄩ儴鏉＄洰鏁寸悊闈㈠悜鐢ㄦ埛鐨勫彉鏇存棩蹇椼€?
   - 娑电洊 v0.1.0 鐨勬柊鍔熻兘锛?5 椤癸級銆佹敼杩涳紙6 椤癸級銆佸凡鐭ラ檺鍒讹紙6 椤癸級銆?

2. **鏂板缓 `docs/USER_GUIDE.md`**
   - 涓枃绠€瑕佺増鐢ㄦ埛鎸囧崡锛氱郴缁熻姹傘€佸畨瑁呰鏄庛€佷笁姝ユ仮澶嶅揩閫熷紑濮嬨€佸姛鑳借鏄庯紙鎵弿/鎭㈠/棰勮/淇/闀滃儚/鍋ュ悍鐩戞祴/浼氳瘽/鑷畾涔夌鍚嶏級銆?

3. **鏂板缓 `docs/FAQ.md`**
   - 12 涓父瑙侀棶棰樿В绛旓紝瑕嗙洊鎵弿鏃堕暱銆佹仮澶嶅畬鏁存€с€佸悓鍗蜂繚鎶ゃ€佹枃浠剁郴缁熸敮鎸併€佽嚜瀹氫箟绛惧悕銆佽法骞冲彴銆佸彇娑堟搷浣溿€佸彧璇诲畨鍏ㄦ€с€佹枃浠跺悕涔辩爜銆佹牸寮忓寲鎭㈠銆丼SD 鎭㈠銆佷細璇濅繚瀛樸€?

4. **鏂板缓鏍圭洰褰?`README.md`**
   - 椤圭洰绠€浠嬨€佸姛鑳界壒鎬у垪琛ㄣ€佹埅鍥惧崰浣嶃€佸揩閫熷紑濮嬨€佹瀯寤鸿鏄庛€佹枃妗ｇ储寮曡〃銆佹灦鏋勬瑙堝浘銆佽础鐚寚鍗椼€佽鍙瘉澹版槑銆?

5. **鏂板缓 `scripts/Build-Release.ps1`**
   - PowerShell 涓€閿彂甯冩瀯寤鸿剼鏈細CMake 閰嶇疆 鈫?缂栬瘧 鈫?澶嶅埗鍙墽琛屾枃浠?鈫?windeployqt 鎵撳寘 Qt 渚濊禆 鈫?澶嶅埗 LICENSE/README/docs 鈫?杈撳嚭鍙戝竷鍖呮憳瑕併€?
   - 鏀寔鍙傛暟锛歚-QtPrefix`銆乣-Config`銆乣-OutputDir`銆乣-SkipBuild`銆乣-SkipDeploy`銆?

6. **鏂板缓鏍圭洰褰?`LICENSE`**
   - MIT 璁稿彲璇侊紝鐗堟潈 Copyright (c) 2026 PhoenixRecover Contributors銆?
   - 鏈熬闄勭涓夋柟澹版槑锛歈t (LGPL v3, 鍔ㄦ€侀摼鎺?銆丗Fmpeg (LGPL/GPL, 鐙珛杩涚▼璋冪敤)銆?

### 娑夊強鏂囦欢

| 鏂囦欢 | 鍙樻洿绫诲瀷 |
|------|---------|
| `docs/CHANGELOG.md` | 鏂板缓 |
| `docs/USER_GUIDE.md` | 鏂板缓 |
| `docs/FAQ.md` | 鏂板缓 |
| `README.md` | 鏂板缓 |
| `scripts/Build-Release.ps1` | 鏂板缓 |
| `LICENSE` | 鏂板缓 |
| `docs/DEVELOPMENT_LOG.md` | 淇敼锛堟坊鍔犳湰鏉¤褰曪級 |

### 璁捐瑕佺偣

- 鎵€鏈夐潰鍚戠敤鎴锋枃妗ｄ娇鐢ㄤ腑鏂囨挵鍐欙紝涓庨」鐩棦鏈夋枃妗ｉ鏍肩粺涓€銆?
- `Build-Release.ps1` 璁捐鍙傜収 `Regress.ps1` 鐨勫弬鏁伴鏍煎拰閿欒澶勭悊妯″紡锛屼繚鎸佽剼鏈綋绯讳竴鑷存€с€?
- LICENSE 绗笁鏂瑰０鏄庢鏄庣‘浜?Qt 鍔ㄦ€侀摼鎺ュ拰 FFmpeg 鐙珛杩涚▼璋冪敤鐨勫悎瑙勬柟寮忋€?
- RELEASE_CHECKLIST.md 涓姹傜殑銆岃鍙瘉鏂囦欢瀹屾暣銆嶃€孋HANGELOG.md 宸叉洿鏂般€嶃€屾洿鏂伴」鐩?README銆嶄笁椤瑰潎宸插畬鎴愩€?

---

## LOG-20260526-007

| 瀛楁 | 鍐呭 |
|------|------|
| 鏃ユ湡 | 2026-05-26 |
| 绫诲瀷 | 娴嬭瘯宸ョ▼鍖栵紙鎬ц兘鍩哄噯鏂囨。 + 娴嬭瘯鎵ц鐭╅樀澧炲己 + 绔炲搧瀵规瘮琛ㄦ牸瀵归綈锛?|
| 鍏宠仈閲岀▼纰?| TECHNICAL_PLAN 搂5銆乀EST_PLAN 搂4.3銆丳ERFORMANCE_BASELINE |

### 鍙樻洿鎽樿

1. **鏂板缓 `docs/PERFORMANCE_BASELINE.md`**  
   - 瀹氫箟鎬ц兘鍩哄噯鏂囨。缁撴瀯锛氭祴璇曠幆澧冩ā鏉裤€佹壂鎻忔€ц兘鍩哄噯锛堜笁闃舵鍒嗘璁℃椂锛夈€佹仮澶嶆€ц兘鍩哄噯銆乁I 鍝嶅簲鎬у熀鍑嗐€佸洖褰掑姣旇鍒欍€佺珵鍝佸弬鑰冨姣斻€? 
   - 鏄庣‘鍥為€€闃堝€硷細鑰楁椂澧炲姞 >20% 鎴栧唴瀛樺鍔?>50% 瀹氫箟涓烘樉钁楀洖閫€锛岄樆鏂彂鐗堛€? 
   - 鎸?ThirdParty_Comparison_TestPlan.md 鐨?5 涓?Case 棰勭暀绔炲搧瀵规瘮璁板綍琛ㄣ€?

2. **鏇存柊 `docs/TEST_EXECUTION_MATRIX.md`**  
   - 涓烘墽琛岀煩闃佃〃鏍兼柊澧?鑷姩鍖栫姸鎬?鍜?鏈€杩戞墽琛?涓ゅ垪銆? 
   - 鏍囨敞 SCN-001 宸叉湁 CTest 瑕嗙洊锛圖FTT_FAT_Undelete / DFTT_NTFS_Undelete锛夈€? 
   - 鏍囨敞 RCV-002 涓哄崐鑷姩锛圕ompare-RecoveryResults.ps1 -Strict锛夈€? 
   - 鍏朵綑 14 涓敤渚嬫爣娉ㄤ负"鎵嬪伐鎵ц"銆? 
   - 鏂板鑷姩鍖栬鐩栫巼缁熻琛紙CTest 6% / 鍗婅嚜鍔?6% / 鎵嬪伐 88%锛夈€? 
   - 娣诲姞 PERFORMANCE_BASELINE.md 浜ゅ弶寮曠敤銆?

3. **鏇存柊 `ThirdParty_Comparison_ResultSheet.md`**  
   - 閲嶆瀯涓烘寜 TestPlan 鐨?5 涓?Case 鍒嗚妭缁勭粐锛圢TFS 鍩虹瀵规瘮銆佸凡鍒犻櫎鏂囦欢銆佸ぇ缁撴灉閲忋€佺被鍨嬫爲/璺緞鏍戙€佸仠姝笌寮傚父锛夈€? 
   - 姣忎釜 Case 鐙珛璁板綍瀛愯〃鏍硷紙鍚姩琛ㄧ幇銆佹暟閲忋€佹€ц兘銆佽川閲忥級銆? 
   - 鎵€鏈夊瓧娈甸濉?(寰呭～鍐?"妯℃澘銆? 
   - 鏂板鍘嗗彶杞璁板綍绱㈠紩琛ㄣ€?

### 娑夊強鏂囦欢

| 鏂囦欢 | 鍙樻洿绫诲瀷 |
|------|---------|
| `docs/PERFORMANCE_BASELINE.md` | 鏂板缓 |
| `docs/TEST_EXECUTION_MATRIX.md` | 淇敼锛堝鍔犺嚜鍔ㄥ寲/鎵ц鍒楋級 |
| `ThirdParty_Comparison_ResultSheet.md` | 閲嶅啓锛堝榻?TestPlan Case 缁撴瀯锛?|

### 璁捐瑕佺偣

- 鎬ц兘鍩哄噯鏂囨。涓?TEST_PLAN 搂4锛圠3 鎬ц兘娴嬭瘯锛変簰琛ワ細TEST_PLAN 瀹氫箟娴嬭瘯绛栫暐锛孭ERFORMANCE_BASELINE 鎻愪緵璁板綍妯℃澘鍜屽洖閫€鍒ゅ畾鏍囧噯銆?
- 鎵ц鐭╅樀鏂板鍒椾娇姣忎釜鐢ㄤ緥鐨勮嚜鍔ㄥ寲鐘舵€佸拰鎵ц鍘嗗彶涓€鐩簡鐒讹紝渚夸簬璇嗗埆娴嬭瘯鐩插尯銆?
- 绔炲搧瀵规瘮缁撴灉琛ㄦ寜 Case 鍒嗚妭鍚庯紝姣忔鎵ц鍙渶濉啓瀵瑰簲 Case 鐨勫瓙琛紝閬垮厤鏁磋〃澶嶅埗閫犳垚鐨勫啑浣欍€?

---

## LOG-20260526-006

| 瀛楁 | 鍐呭 |
|------|------|
| 鏃ユ湡 | 2026-05-26 |
| 绫诲瀷 | 娴嬭瘯宸ョ▼鍖栵紙DFTT 闀滃儚绔埌绔泦鎴愭祴璇?+ 娴嬭瘯璁″垝鏂囨。锛?|
| 鍏宠仈閲岀▼纰?| TECHNICAL_PLAN 搂5銆乀EST_PLAN |

### 鍙樻洿鎽樿

1. **鏂板缓 `tests/integration/` 鐩綍锛屾坊鍔犱袱涓?DFTT 闀滃儚闆嗘垚娴嬭瘯**  
   - `test_dftt_fat_undelete.cpp`锛氭墦寮€ `testdata/dftt/6-undel-fat/6-fat-undel.dd`锛岄€氳繃 `SectorReader::OpenImage()` 鈫?`FatParser::Open()` 鈫?`ScanMetadata()` 鈫?`GetAllFiles()` 楠岃瘉宸插垹闄ゆ枃浠讹紙sing.dat銆乵ult1.dat銆乫rag1.dat銆乫rag2.dat銆乵ult2.dat銆乫rag3.dat锛夌殑鍙戠幇涓庡ぇ灏忓尮閰嶏紝浠ュ強 `dir1/`銆乣dir1/dir2/` 鐩綍鑺傜偣瀛樺湪銆? 
   - `test_dftt_ntfs_undelete.cpp`锛氭墦寮€ `testdata/dftt/7-undel-ntfs/7-ntfs-undel.dd`锛岄獙璇侀┗鐣欐枃浠?`res1.dat`锛?01 B锛宍residentData` 闈炵┖锛夈€佹牴绾ф枃浠讹紙sing1.dat銆乵ult1.dat銆乫rag1.dat銆乫rag2.dat锛夈€佸瓙鐩綍鏂囦欢锛坉ir1/mult2.dat銆乨ir1/dir2/frag3.dat銆乨ir3/sing2.dat锛夌殑鍙戠幇涓庡ぇ灏忓尮閰嶃€?

2. **鏇存柊 `tests/CMakeLists.txt`**  
   - 鍦?`WIN32 AND PHOENIX_BUILD_GUI` 鏉′欢涓嬫柊澧炰袱涓彲鎵ц鐩爣 `test_dftt_fat` 鍜?`test_dftt_ntfs`銆? 
   - 閾炬帴 SectorReader_win銆丗atParser/FatStructs锛團AT锛夈€丯tfsParser/UsnJournalParser锛圢TFS锛夈€乁tils銆丩ogger銆? 
   - 閾炬帴 `Qt5::Core` 鍜?`ole32`銆? 
   - 閫氳繃 `PHOENIX_SOURCE_DIR` 缂栬瘧瀹忎紶閫掗」鐩牴璺緞锛岃В鏋愭祴璇曟暟鎹浉瀵逛綅缃€? 
   - 娉ㄥ唽 CTest锛歚DFTT_FAT_Undelete`銆乣DFTT_NTFS_Undelete`銆?

3. **鏂板缓 `docs/TEST_PLAN.md`**  
   - 涓枃鎾板啓瀹屾暣娴嬭瘯璁″垝锛氭祴璇曠瓥鐣ユ杩帮紙L1鈥揕4 鍥涘眰绾э級銆佸崟鍏冩祴璇曟竻鍗曪紙6 涓幇鏈夋祴璇曪級銆侀泦鎴愭祴璇曟竻鍗曪紙DFTT FAT/NTFS 闀滃儚娴嬭瘯璇︾粏楠岃瘉椤癸級銆佹€ц兘娴嬭瘯璁″垝锛堝満鏅?鎸囨爣/闃堝€硷級銆佹墜宸ラ獙鏀舵祴璇曡鍒掞紙P0 蹇呮祴 + P1 鎵╁睍锛夈€佸洖褰掓祴璇曟祦绋嬶紙Regress.ps1 + Compare-RecoveryResults.ps1锛夈€?

### 娑夊強鏂囦欢

| 鏂囦欢 | 鍙樻洿绫诲瀷 |
|------|---------|
| `tests/integration/test_dftt_fat_undelete.cpp` | 鏂板缓 |
| `tests/integration/test_dftt_ntfs_undelete.cpp` | 鏂板缓 |
| `tests/CMakeLists.txt` | 淇敼锛堟坊鍔犻泦鎴愭祴璇曠洰鏍囷級 |
| `docs/TEST_PLAN.md` | 鏂板缓 |

### 璁捐瑕佺偣

- **骞冲彴闅旂**锛氭祴璇曟枃浠朵娇鐢?`#if PHOENIX_OS_WINDOWS` 鍖呰９鍏ㄩ儴瀹炵幇锛岄潪 Windows 骞冲彴缂栬瘧涓?`SKIP` 妗┿€?
- **璺緞瑙ｆ瀽**锛氶€氳繃 CMake 浼犲叆 `PHOENIX_SOURCE_DIR` 瀹忥紝閬垮厤娴嬭瘯浜岃繘鍒舵枃浠朵笌婧愮爜鐩綍浣嶇疆纭€﹀悎銆?
- **杞婚噺娴嬭瘯妗嗘灦**锛氫娇鐢?`CHECK`/`CHECK_MSG` 瀹忔浛浠ｅ閮ㄦ祴璇曟鏋朵緷璧栵紝淇濇寔闆堕澶栦緷璧栥€?
- **棰勬湡鏁版嵁鏉ユ簮**锛氶獙璇佸熀绾挎潵鑷?`testdata/manifests/expected_results.csv`锛圖FTT 瀹樻柟鏂囨。锛夈€?

### 楠岃瘉璁板綍

- 浠ｇ爜宸查€氳繃闈欐€佸鏌ワ紝缂栬瘧楠岃瘉闇€鍦ㄩ厤缃?Qt5 + MSVC 鐨?Windows 鐜涓墽琛屻€?

---

## LOG-20260526-005

| 瀛楁 | 鍐呭 |
|------|------|
| 鏃ユ湡 | 2026-05-26 |
| 绫诲瀷 | 鏋舵瀯閲嶆瀯锛圧ecoveryScanPage 瀛愮粍浠舵媶鍒?+ 璧勬簮濉厖锛?|
| 鍏宠仈閲岀▼纰?| STYLE_GUIDE 搂椤甸潰妯″潡鍖?|

### 鍙樻洿鎽樿

1. **鏂板 4 涓瓙缁勪欢锛坄pages/widgets/`锛?*  
   - `ScanProgressBar.h/.cpp`锛氬皝瑁呮壂鎻忚繘搴︽樉绀猴紙杩涘害鏉?+ 鐧惧垎姣?+ 闃舵鏂囨湰 + 宸叉壂鎻忓瓧鑺?+ 閫熷害 + 鑰楁椂锛夛紝鍏紑 `setProgress()`銆乣reset()` 鏂规硶銆? 
   - `PreviewPanel.h/.cpp`锛氬皝瑁呭彸渚ч瑙堥潰鏉匡紙QListWidget 缃戞牸棰勮 + 缂╃暐鍥惧紓姝ュ姞杞斤級锛屽叕寮€ `showFilePreview()`銆乣clear()`锛屽唴閮ㄥ鐞?MediaThumbnails 璋冪敤銆? 
   - `ScanFooterBar.h/.cpp`锛氬皝瑁呭簳閮ㄦ搷浣滄爮锛堟仮澶嶆寜閽?+ 淇濈暀鐩綍缁撴瀯澶嶉€夋 + 鏂囦欢缁熻锛夛紝淇″彿 `recoverClicked()`銆乣exportClicked()`銆? 
   - `FilterPanel.h/.cpp`锛氬皝瑁呯瓫閫夐潰鏉匡紙鎼滅储妗?+ 鎼滅储鎸夐挳锛夛紝淇″彿 `filterChanged(keyword)`銆?

2. **RecoveryScanPage.h 鏇存柊**  
   - 鏂板瀛愮粍浠舵寚閽堟垚鍛橈紙`scanProgressBar_`銆乣previewPanel_`銆乣scanFooterBar_`銆乣filterPanel_`锛夈€? 
   - 鏂板 `onFilterChanged(const QString &)` 绉佹湁妲姐€? 
   - 娣诲姞 `ScanResultModel` / `ScanResultFilterModel` / `QTableView` 鍓嶅悜澹版槑涓庢垚鍛樸€?

3. **RecoveryScanPage.cpp 鏇存柊**  
   - 鏂板 `#include` 寮曞叆 4 涓瓙缁勪欢澶存枃浠躲€? 
   - 瀛愮粍浠朵负鍚庣画杩唬涓€愭鏇挎崲鍐呰仈 UI 浠ｇ爜鍋氬噯澶囥€?

4. **濉厖 `PhoenixRecover.qrc`**  
   - 鏂板缓 `resources/icons/` 鐩綍锛屾坊鍔?11 涓?SVG 鍗犱綅鍥炬爣锛坅pp_icon銆乻can銆乺ecover銆乺epair銆乮maging銆乻mart銆乫older銆乫ile銆乧heck銆亀arning銆乪rror锛夈€? 
   - 鏂板缓 `resources/styles/main.qss`锛堟祬鑹蹭富棰樺熀纭€鏍峰紡琛級锛岃鐩栧叏灞€瀛椾綋銆佹爲銆佽〃鏍笺€佹粴鍔ㄦ潯銆佹爣绛鹃〉銆佽繘搴︽潯銆佹寜閽€佽緭鍏ユ銆? 
   - 鏇存柊 `.qrc` 寮曠敤鎵€鏈?12 涓祫婧愭枃浠躲€?

5. **鏇存柊 `CMakeLists.txt`**  
   - 鍦?`PHOENIX_SOURCES` 娣诲姞 4 涓柊鐨勫瓙缁勪欢 `.cpp` 鏂囦欢銆?

### 璁捐璇存槑

- 鏈涓哄閲忛噸鏋勭涓€姝ワ細瀛愮粍浠跺凡鍒涘缓骞跺彲缂栬瘧锛孯ecoveryScanPage 鐨勫唴鑱?UI 浠ｇ爜浠嶄繚鐣欎互纭繚涓嶇牬鍧忕幇鏈夊姛鑳姐€? 
- 鍚庣画杩唬灏嗛€愭鎶婃瀯閫犲嚱鏁颁腑鐨?UI 鍒涘缓閫昏緫濮旀墭缁欏瓙缁勪欢锛屽苟鎶婄缉鐣ュ浘璋冨害/鏍戠鐞嗙瓑鐙珛閫昏緫杩佺Щ鍒板搴斿瓙绫伙紝鏈€缁堝皢 RecoveryScanPage.cpp 浠?~4400 琛岄檷鑷?<1500 琛屻€? 
- SVG 鍥炬爣涓鸿璁″崰浣嶇锛屽悗缁彲鏇挎崲涓轰笓涓氳璁＄銆?

---

## LOG-20260526-004

| 瀛楁 | 鍐呭 |
|------|------|
| 鏃ユ湡 | 2026-05-26 |
| 绫诲瀷 | 鏋舵瀯閲嶆瀯锛圧ecoveryFlowController 椤甸潰璺敱鐘舵€佹満瀹炵幇锛?|
| 鍏宠仈閲岀▼纰?| DESIGN_RecoveryFlowController |

### 鍙樻洿鎽樿

1. **`RecoveryFlowController.h` 澶у箙閲嶆瀯**  
   - 鏂板 `FlowState` 鏋氫妇锛圛dle / LocationSelect / Scanning / Results / RecoveryTarget / RecoveryComplete / SDCard / MoreTools / Imaging / Repair / SmartHealth锛夈€? 
   - 鏂板 `init()` 鏂规硶锛屾帴鏀舵墍鏈夐〉闈㈡寚閽堝拰鎺у埗鍣ㄦ寚閽堬紝缁熶竴寤虹珛淇″彿杩炴帴銆? 
   - 鏂板瀵艰埅鏂规硶锛歚navigateToLocation()`銆乣navigateToSDCard()`銆乣navigateToMoreTools()`銆乣startScan()`銆乣showResults()`銆乣startRecovery()`銆乣showComplete()`銆乣navigateBack()`銆? 
   - 鏂板浼氳瘽绠＄悊锛歚saveCurrentSession()`銆乣tryLoadLastSession()`銆乣hasUnsavedSession()`銆? 
   - 鏂板淇″彿 `stateChanged(FlowState)` 鍜屼晶鏍忚矾鐢辨Ы `onSidebarChanged(int)`銆?

2. **`RecoveryFlowController.cpp` 瀹屾暣閲嶅啓**  
   - `init()`锛氬缓绔?LocationPage / SDCardPage / ToolsPage / ScanPage / ScanController 涔嬮棿鐨勬墍鏈変俊鍙疯繛鎺ャ€? 
   - `onStartScanRequested()`锛氶獙璇佺鐩樹俊鎭?鈫?鍒囨崲 outerStack 鈫?璋冪敤 `scanCtrl->startVolumeScan()` 鈫?`setState(Scanning)`銆? 
   - `onScanPageBack()`锛氬仠姝㈡壂鎻?鈫?鍥為€€鍒?prevState\_銆? 
   - `onScanFinished()`锛歚setState(Results)` 鈫?鑷姩 `saveCurrentSession()`銆? 
   - `onSidebarChanged()`锛氭牴鎹?pageIndex 璺敱锛屾壂鎻忎腑鍒囨崲闇€纭銆? 
   - `saveCurrentSession()`锛氫粠 `ScanResultStore` 鎻愬彇鍏ㄩ噺 `FileNode` + `Partition` 鈫?`ScanSerializer::Save()` 鈫?璁板叆 `QSettings`銆? 
   - `tryLoadLastSession()`锛氳鍙?QSettings 鈫?楠岃瘉鏂囦欢 鈫?`ScanSerializer::Load()` 鈫?濉厖 Store 鈫?鍒囨崲鍒?Results銆?

3. **`PhoenixRecover.h` 灏忓箙淇敼**  
   - 绉婚櫎 `handleStartScanRequest()` 澹版槑锛屾柊澧?`closeEvent()` 瑕嗙洊銆?

4. **`PhoenixRecover.cpp` 涓箙淇敼**  
   - 绉婚櫎鍒嗘暎鐨勪俊鍙疯繛鎺ュ拰 `handleStartScanRequest()` 瀹炵幇銆? 
   - 鏂板 `controller_->init(...)` 缁熶竴鍒濆鍖栵紝渚ф爮璺敱杞彂鍒?`controller_->onSidebarChanged()`銆? 
   - 鏂板 `closeEvent()` 鏈繚瀛樹細璇濇彁绀恒€?

5. **`ScanResultStore` 灏忓箙鎵╁睍**  
   - 鏂板 `allRecoveredFileNodes()` 鏂规硶鐢ㄤ簬搴忓垪鍖栥€?

### 璁捐璇存槑

- 鎵€鏈夐〉闈㈣矾鐢卞拰娴佺▼閫昏緫鏀跺綊 `RecoveryFlowController`锛屼富绐楀彛鍙礋璐?UI 鍒涘缓涓庡竷灞€銆? 
- 椤甸潰鍙彂灏勪俊鍙凤紝涓嶅仛涓氬姟閫昏緫銆? 
- SD 鍗℃娴嬪拰鏇村鎭㈠宸ュ叿瀛愭祦绋嬪湪鐘舵€佹満涓鐣欑姸鎬佷綅锛屽綋鍓嶄互鍗犱綅瀹炵幇銆?

---

## LOG-20260526-004

| 瀛楁 | 鍐呭 |
|------|------|
| 鏃ユ湡 | 2026-05-26 |
| 绫诲瀷 | 鏋舵瀯閲嶆瀯锛堣法骞冲彴鍩虹锛歋ectorReader 鍜?DiskManager 骞冲彴鎷嗗垎锛?|
| 鍏宠仈閲岀▼纰?| DESIGN_CrossPlatform 搂2-4 |

### 鍙樻洿鎽樿

1. **SectorReader 骞冲彴鎷嗗垎**
   - `disk/SectorReader.h`锛氬紩鍏?`common/Platform.h`锛岀敤 `#if PHOENIX_OS_WINDOWS` 鏉′欢缂栬瘧闅旂 Windows 鐗瑰畾鎴愬憳锛坄HANDLE`銆乣OVERLAPPED` 鐩稿叧锛夛紱Unix 骞冲彴浣跨敤 `int m_fd = -1` 鍜?`m_lastErrno`銆?
   - `disk/SectorReader.cpp` 鈫?`disk/SectorReader_win.cpp`锛氶噸鍛藉悕骞剁敤 `#if PHOENIX_OS_WINDOWS` 鍖呰９鏁翠釜瀹炵幇銆?
   - 鏂板缓 `disk/SectorReader_unix.cpp`锛歚#if PHOENIX_OS_LINUX_DESKTOP || PHOENIX_OS_MACOS` 鍖呰９锛屼娇鐢?POSIX API锛坄open`/`pread`/`close`锛夛紝Linux 璁惧璇诲彇浣跨敤 `O_DIRECT` + `posix_memalign` 瀵归綈缂撳啿锛宮acOS 浣跨敤鏅€?`pread`銆傚嚑浣曟煡璇娇鐢?`BLKSSZGET`/`BLKGETSIZE64`锛圠inux锛夊拰 `DKIOCGETBLOCKSIZE`/`DKIOCGETBLOCKCOUNT`锛坢acOS锛夈€傚潖鎵囧尯澶勭悊涓庡彇娑堟爣蹇楁鏌ヤ笌 Windows 鐗堜竴鑷淬€?

2. **DiskManager 骞冲彴鎷嗗垎**
   - `disk/DiskManager.h`锛氬紩鍏?`common/Platform.h`锛屽皢 Windows 鐗瑰畾绉佹湁鏂规硶锛坄DetectFsType(HANDLE,...)`銆乣AssignDriveLetters`銆乣QuerySmart`锛夌敤 `#if PHOENIX_OS_WINDOWS` 鍖呰９銆?
   - `disk/DiskManager.cpp` 鈫?`disk/DiskManager_win.cpp`锛氶噸鍛藉悕骞剁敤 `#if PHOENIX_OS_WINDOWS` 鍖呰９銆?
   - 鏂板缓 `disk/DiskManager_unix.cpp`锛歚GetDisks()` 鍦?Linux 涓婇亶鍘?`/sys/block/` 鏋氫妇鐗╃悊纾佺洏锛坰d*/nvme*/hd*/vd*锛夛紝璇诲彇 sysfs 灞炴€ц幏鍙栧ぇ灏忋€佸瀷鍙枫€佸巶鍟嗙瓑淇℃伅锛沵acOS 涓婇亶鍘?`/dev/disk*` 骞堕€氳繃 ioctl 鑾峰彇鍑犱綍淇℃伅銆俙LoadImageFile()` 浣跨敤 POSIX 鏂囦欢 API 骞跺鐢?MBR 鍒嗗尯琛ㄨВ鏋愰€昏緫銆?

3. **PartitionProbe 鏉′欢缂栬瘧**
   - `disk/PartitionProbe.cpp`锛氬皢椤堕儴 `#include <Windows.h>` 鏀逛负鏉′欢鍖呭惈锛坄#if PHOENIX_OS_WINDOWS`锛夈€侾artitionProbe 鏈韩涓嶇洿鎺ヨ皟鐢?Windows API锛堟墍鏈?I/O 閫氳繃 SectorReader锛夛紝鏁版嵁瑙ｆ瀽閫昏緫锛圡BR/GPT/VBR/ext4锛夊ぉ鐒惰法骞冲彴銆?

4. **CMakeLists.txt 鏇存柊**
   - 鏂板 `PHOENIX_PLATFORM_SOURCES` 鍙橀噺锛屾牴鎹?`WIN32` 鏉′欢閫夋嫨 `*_win.cpp` 鎴?`*_unix.cpp`銆?
   - 浠庡浐瀹氬垪琛ㄤ腑绉婚櫎鍘熸湁鐨?`disk/SectorReader.cpp` 鍜?`disk/DiskManager.cpp`銆?

### 娑夊強鏂囦欢

| 鏂囦欢 | 鍙樻洿绫诲瀷 |
|------|---------|
| `disk/SectorReader.h` | 淇敼锛堟潯浠剁紪璇戦殧绂诲钩鍙扮被鍨嬶級 |
| `disk/SectorReader.cpp` 鈫?`disk/SectorReader_win.cpp` | 閲嶅懡鍚?+ 鍖呰９ `#if` |
| `disk/SectorReader_unix.cpp` | 鏂板缓锛圥OSIX 瀹炵幇锛?|
| `disk/DiskManager.h` | 淇敼锛堟潯浠剁紪璇戦殧绂?Windows 绉佹湁鏂规硶锛?|
| `disk/DiskManager.cpp` 鈫?`disk/DiskManager_win.cpp` | 閲嶅懡鍚?+ 鍖呰９ `#if` |
| `disk/DiskManager_unix.cpp` | 鏂板缓锛圲nix 瀹炵幇锛?|
| `disk/PartitionProbe.cpp` | 淇敼锛圵indows.h 鏉′欢鍖呭惈锛?|
| `CMakeLists.txt` | 淇敼锛堝钩鍙版潯浠堕€夋嫨婧愭枃浠讹級 |

### 璁捐瑕佺偣

- **缂栬瘧闅旂**锛歐indows 鏋勫缓浠呯紪璇?`*_win.cpp`锛孶nix 鏋勫缓浠呯紪璇?`*_unix.cpp`锛屼笉瀛樺湪浜ゅ弶渚濊禆銆?
- **澶存枃浠舵棤娉勬紡**锛歚SectorReader.h` 鍜?`DiskManager.h` 涓嶅啀鍦ㄩ潪 Windows 骞冲彴鍖呭惈 `<Windows.h>`銆?
- **Unix 瀹炵幇涓?v0**锛氬姛鑳芥€у熀纭€瀹炵幇锛岃兘缂栬瘧閫氳繃骞舵敮鎸侀暅鍍忔枃浠惰鍙栵紝鐗╃悊纾佺洏鏋氫妇鍦?Linux 涓婅緝瀹屾暣锛坰ysfs锛夛紝macOS 涓烘渶灏忓寲瀹炵幇銆?
- **鍚戝悗鍏煎**锛歐indows 骞冲彴鐨勬瀯寤鸿涓哄拰杩愯鏃惰涓轰笉鍙楀奖鍝嶃€?

### 楠岃瘉璁板綍

- 浠ｇ爜灞傞潰宸茬‘淇濈紪璇戦殧绂绘纭細`#if` 瀹忔潯浠朵笌 `common/Platform.h` 瀹氫箟涓€鑷淬€?
- 瀹為檯缂栬瘧楠岃瘉闇€鍦ㄥ悇骞冲彴鐜涓墽琛屻€?

---

## LOG-20260526-003

| 瀛楁 | 鍐呭 |
|------|------|
| 鏃ユ湡 | 2026-05-26 |
| 绫诲瀷 | 鍔熻兘锛堟柊澧炴枃浠朵慨澶嶅瓙绯荤粺 v0锛?|
| 鍏宠仈閲岀▼纰?| DESIGN_RepairSubsystem 搂2-4 |

### 鍙樻洿鎽樿

1. **鏂板缓 `repair/` 妯″潡鐩綍锛屽疄鐜颁慨澶嶅瓙绯荤粺 v0**  
   - `repair/RepairTypes.h`锛氫慨澶嶇粨鏋滅粨鏋勪綋 `RepairResult`銆? 
   - `repair/IFileRepair.h`锛氱粺涓€淇鎺ュ彛锛坄formatName` / `supportedExtensions` / `canRepair` / `repair`锛夈€? 
   - `repair/RepairManager.h/.cpp`锛氫慨澶嶅櫒娉ㄥ唽涓庢煡鎵剧鐞嗗櫒锛屾瀯閫犳椂鑷姩娉ㄥ唽 ZIP 鍜?MP4 淇鍣ㄣ€? 
   - `repair/ZipRepair.h/.cpp`锛歓IP 淇瀹炵幇鈥斺€旀壂鎻忔湰鍦版枃浠跺ご銆佸鐞?Data Descriptor銆侀噸寤轰腑蹇冪洰褰曚笌 EOCD銆? 
   - `repair/Mp4Repair.h/.cpp`锛歁P4 淇瀹炵幇鈥斺€旇В鏋愰《绾?box銆乵oov 閲嶅畾浣嶏紙鍚?stco/co64 鍋忕Щ淇锛夈€丗Fmpeg remux 闄嶇骇绛栫暐銆?

2. **鏇存柊 `CMakeLists.txt`**  
   - 鍦?`PHOENIX_SOURCES`锛坄PHOENIX_BUILD_GUI` 鏉′欢鍐咃級娣诲姞 `repair/RepairManager.cpp`銆乣repair/ZipRepair.cpp`銆乣repair/Mp4Repair.cpp`銆?

### 璁捐瑕佺偣

- **ZIP 淇**锛氫粠鏂囦欢璧峰鎵弿鎵€鏈?`PK\x03\x04` 鏈湴鏂囦欢澶达紝瑙ｆ瀽鏂囦欢鍚嶃€佸帇缂╂柟寮忋€丆RC32銆佸帇缂╁ぇ灏忥紝鏀寔 Data Descriptor 妯″紡锛涢噸寤轰腑蹇冪洰褰曟潯鐩苟杩藉姞 EOCD锛屽啓鍏ョ嫭绔嬭緭鍑烘枃浠躲€?
- **MP4 淇**锛氱瓥鐣?A 瑙ｆ瀽椤剁骇 box 閾撅紝鑻?moov 鍦?mdat 涔嬪悗鍒欏皢鍏堕噸瀹氫綅鍒板墠閮ㄥ苟璋冩暣 stco/co64 chunk offset锛涚瓥鐣?B 鍦?moov 缂哄け鏃堕檷绾т负 FFmpeg remux锛坄-c copy -movflags +faststart`锛夈€?
- **闈炵牬鍧忔€?*锛氬缁堜繚鐣欏師鏂囦欢涓嶅姩锛屼慨澶嶈緭鍑哄埌鏂版枃浠讹紱鏀寔 `std::atomic<bool>*` 鍙栨秷鏍囧織銆?
- **v0 闄愬埗**锛氫笉澶勭悊鍔犲瘑 ZIP銆乑IP64銆佸垎鍗?ZIP锛汳P4 涓嶅鐞嗗姞瀵嗗唴瀹规垨瀹屽叏鏃?box 缁撴瀯鐨勮８鏁版嵁銆?

### 楠岃瘉璁板綍

- 浠ｇ爜缂栬瘧渚濊禆 Qt5 鐜锛屾湰娆′负缁撴瀯瀹炵幇钀藉湴锛岀紪璇戦獙璇侀渶鍦ㄩ厤缃簡 Qt5 鐨勫紑鍙戠幆澧冩墽琛屻€?

---

## LOG-20260526-002

| 瀛楁 | 鍐呭 |
|------|------|
| 鏃ユ湡 | 2026-05-26 |
| 绫诲瀷 | 鍔熻兘瀹炵幇锛堢粺涓€鎵弿涓夐樁娈垫祦姘寸嚎锛?|
| 鍏宠仈閲岀▼纰?| TECHNICAL_PLAN 搂6 闃舵 2锛堢粺涓€鎵弿娴佹按瀹屽杽锛夈€丏ESIGN_UnifiedScanPipeline |

### 鍙樻洿鎽樿

1. **鏂板 `UnifiedScanConfig` 缁撴瀯浣?*锛坄scan/UnifiedVolumeScanner.h`锛? 
   - `skipDeepScan` / `skipRawScan`锛氬彲閫夎烦杩囬樁娈?2/3銆? 
   - `maxCarveFileSize`锛氶樁娈?3 绛惧悕闆曞埢鐨勬渶澶ф枃浠跺ぇ灏忛檺鍒讹紙榛樿 100 MB锛夈€? 
   - `targetExtensions`锛氫粎鎵弿鎸囧畾鎵╁睍鍚嶏紙涓虹┖鍒欎笉杩囨护锛夈€?

2. **閲嶅啓 `UnifiedVolumeScanner::Run()`**锛坄scan/UnifiedVolumeScanner.cpp`锛? 
   - 涓茶仈涓夐樁娈垫祦姘寸嚎锛? 
     - 闃舵 1: `VolumeSectorScanner`锛團S 鍏冩暟鎹В鏋愶紝鏉冮噸 30%锛? 
     - 闃舵 2: `DeepScan`锛堟畫鐣?MFT/FAT 鐩綍椤规悳绱紝鏉冮噸 40%锛? 
     - 闃舵 3: `RawScan`锛堢函绛惧悕鍖归厤闆曞埢锛屾潈閲?30%锛? 
   - 姣忛樁娈靛紑濮嬪墠妫€鏌ュ彇娑堟爣蹇楋紝鏀寔鍗虫椂鍙栨秷銆? 
   - 杩涘害妯″瀷锛氬悇闃舵鏈湴鐧惧垎姣旀槧灏勫埌鍏ㄥ眬 0鈫?00% 杩炶疮杩涘害銆? 
   - 闃舵鏍囩锛歚闃舵 1/3 鍏冩暟鎹В鏋恅 / `闃舵 2/3 娣卞害鎵弿` / `闃舵 3/3 绛惧悕闆曞埢` / `鎵弿瀹屾垚`銆?

3. **缁熶竴鍘婚噸鏈哄埗**  
   - FNV-1a 鍝堝笇 `(rawOffset|棣栫皣, fileSize)` 鈫?64 浣嶅幓閲嶉敭銆? 
   - `std::unordered_set<uint64_t>` 瀛樺偍宸叉帴鍙楁枃浠讹紝闃舵 2/3 缁撴灉鍏堟煡闆嗗悎鍐嶆彃鍏ャ€? 
   - 鍚岃矾寰勫幓閲嶏細`std::unordered_map<std::wstring, size_t>` 杈呭姪鍒ゆ柇銆?

4. **Worker 绔€傞厤**锛坄controllers/RecoveryScanWorker.cpp`锛? 
   - 鍦?`scanner.Run()` 鍓嶅垱寤哄苟浼犻€?`UnifiedScanConfig`锛堜娇鐢ㄩ粯璁ゅ€硷紝涓夐樁娈靛叏鎵ц锛夈€? 
   - 鏈€缁堣繘搴︽爣绛句粠 `闃舵 1/1 鍏冩暟鎹В鏋愶紙瀹屾垚锛塦 鏀逛负 `鎵弿瀹屾垚`銆?

5. **鍚戝悗鍏煎**  
   - `SetConfig()` 涓哄彲閫夎皟鐢紱涓嶈皟鐢ㄦ椂榛樿涓夐樁娈靛叏閮ㄦ墽琛屻€? 
   - 鐜版湁鎵€鏈夐€氳繃 `UnifiedVolumeScanner` 鐨勮皟鐢ㄨ矾寰勬棤闇€淇敼鍗冲彲宸ヤ綔銆?

### 娑夊強鏂囦欢

| 鏂囦欢 | 鍙樻洿绫诲瀷 |
|------|---------|
| `scan/UnifiedVolumeScanner.h` | 淇敼锛堟柊澧為厤缃粨鏋勪綋銆佷笁闃舵鏂规硶銆佸幓閲嶆垚鍛橈級 |
| `scan/UnifiedVolumeScanner.cpp` | 閲嶅啓锛堜笁闃舵娴佹按绾垮疄鐜帮級 |
| `controllers/RecoveryScanWorker.cpp` | 淇敼锛堜紶閫掗厤缃€佹洿鏂拌繘搴︽爣绛撅級 |

### 楠岃瘉璁板綍

- 浠ｇ爜灞傞潰楠岃瘉锛氭帴鍙ｅ吋瀹规€у凡閫氳繃璁捐纭繚鈥斺€擿UnifiedScanConfig` 鏈夐粯璁ゅ€硷紝`SetConfig()` 涓哄彲閫夎皟鐢ㄣ€? 
- 瀹為檯缂栬瘧涓庤繍琛屾祴璇曢渶鍦ㄩ厤缃?Qt5 + MSVC 鐜涓嬫墽琛屻€?

---

## LOG-20260526-001

| 瀛楁 | 鍐呭 |
|------|------|
| 鏃ユ湡 | 2026-05-26 |
| 绫诲瀷 | 鍔熻兘瀹炵幇锛圫ignatureDB 鑷畾涔夌鍚嶅姞杞斤級 |
| 鍏宠仈閲岀▼纰?| TECHNICAL_PLAN 搂4锛堢鍚嶆壂鎻?/ 鏂囦欢闆曞埢锛?|

### 鍙樻洿鎽樿

1. **瀹炵幇 `SignatureDB::LoadFromFile()`**锛坄scan/SignatureDB.h` / `.cpp`锛? 
   - 杩斿洖鍊肩敱 `bool` 鏀逛负 `int`锛氭垚鍔熻繑鍥炲姞杞界殑绛惧悕鏁伴噺锛屽け璐ヨ繑鍥?`-1`銆? 
   - 浣跨敤 `QFile` + `QJsonDocument` 瑙ｆ瀽澶栭儴 JSON 绛惧悕鏂囦欢銆? 
   - 鏀寔鍗佸叚杩涘埗瀛楃涓茶嚜鍔ㄨ浆鎹负 `std::vector<uint8_t>`锛堝鐢ㄥ凡鏈夌殑 `HexToBytes`锛夈€? 
   - 閫愭潯鏍￠獙骞惰烦杩囨棤鏁堟潯鐩紙缂哄皯 extension/magic銆乭ex 瑙ｇ爜涓虹┖绛夛級锛岄€氳繃 Logger 璁板綍鍘熷洜銆? 
   - 鏂扮鍚嶈拷鍔犲埌鍐呭缓绛惧悕鏁版嵁搴撴湯灏撅紝涓嶅奖鍝嶅凡鏈夊尮閰嶄紭鍏堢骇銆?

2. **鏂板鑷畾涔夌鍚嶆牸寮忔枃妗?*锛坄docs/SIGNATURE_FORMAT.md`锛? 
   - 瀹氫箟 JSON 绛惧悕鏂囦欢鐨勬牴缁撴瀯锛坄{ "signatures": [...] }`锛夈€? 
   - 閫愬瓧娈佃鏄庣被鍨嬨€佸繀濉€с€侀粯璁ゅ€煎拰鐢ㄩ€斻€? 
   - 鍖呭惈鍗佸叚杩涘埗瀛楃涓茶鍒欏拰瀹屾暣绀轰緥銆?

3. **鏂板绀轰緥绛惧悕鏂囦欢**锛坄testdata/custom_signatures.json`锛? 
   - 鍖呭惈 3 涓ず渚嬬鍚嶏細SQLite 3 鏁版嵁搴撱€丅MP 浣嶅浘銆丱GG 闊抽銆? 
   - 鍙洿鎺ョ敤浜庨泦鎴愭祴璇曢獙璇?`LoadFromFile` 鍔熻兘銆?

4. **API 鍙傝€冩洿鏂?*锛坄docs/API_REFERENCE.md`锛? 
   - 鏇存柊 `SignatureDB::LoadFromFile` 鏂规硶绛惧悕鍜岃繑鍥炲€艰鏄庛€? 
   - 琛ュ厖 JSON 绛惧悕鏂囦欢鏍煎紡寮曠敤閾炬帴銆?

### 楠岃瘉璁板綍

- 浠ｇ爜宸查€氳繃闈欐€佸鏌ワ紝Qt JSON 瑙ｆ瀽璺緞瀹屾暣瑕嗙洊閿欒鍒嗘敮銆? 
- 缂栬瘧楠岃瘉闇€鍦ㄥ凡閰嶇疆 Qt5 鐨勭幆澧冧腑鎵ц銆?

---

## LOG-20260423-002

| 瀛楁 | 鍐呭 |
|------|------|
| 鏃ユ湡 | 2026-04-23 |
| 绫诲瀷 | 宸ョ▼鍖栵紙缂栫爜-娴嬭瘯-鍥炲綊闂幆锛?|
| 鍏宠仈閲岀▼纰?| TECHNICAL_PLAN 搂5銆乀EST_EXECUTION_MATRIX |

### 鍙樻洿鎽樿

1. **`ReadSignedLE` 鍐呰仈鍒?`common/Utils.h`**  
   - 涓?`ReadLE16/32/64` 涓€鑷达紝渚夸簬鍦ㄦ棤 Windows I/O 閾炬帴鐨勭炕璇戝崟鍏冧腑娴嬭瘯涓庡鐢ㄣ€?

2. **鍙€夋瀯寤轰富绋嬪簭锛歚PHOENIX_BUILD_GUI`**  
   - `CMakeLists.txt` 澧炲姞 `option(PHOENIX_BUILD_GUI ... ON)`锛沗OFF` 鏃朵笉 `find_package(Qt5)`銆佷笉鐢熸垚 `PhoenixRecover` 鍙墽琛屾枃浠讹紝浠呮瀯寤?`tests/` 绛夋棤 Qt 鐩爣锛屼究浜庢棤 Qt 鐜鎴栫函鏍稿績搴撳洖褰掋€?

3. **鏂板 CTest 鍗曟祴鐩爣**  
   - `tests/test_utils_numeric.cpp`锛氭牎楠屽皬绔鍐欍€佸榻愩€乣ReadSignedLE` 绗﹀彿鎵╁睍銆? 
   - 鏍?`CMakeLists.txt` 鍚敤 `enable_testing()` 骞?`add_subdirectory(tests)`銆?

4. **鍥炲綊鍏ュ彛鑴氭湰**  
   - `scripts/Regress.ps1`锛歚cmake` 閰嶇疆/缂栬瘧 鈫?`ctest` 鈫?鍙€夊鎭㈠鐩綍鎵ц `Compare-RecoveryResults.ps1 -Strict`锛堝瓙杩涚▼璋冪敤锛岄伩鍏?`exit` 缁堟鐖朵細璇濓級銆? 
   - `-TestsOnly`锛氫紶鍏?`-DPHOENIX_BUILD_GUI=OFF`锛屼粎璺戝崟娴嬮摼璺€?

5. **瀵规瘮鑴氭湰閫€鍑虹爜**  
   - `testdata/tools/Compare-RecoveryResults.ps1` 澧炲姞 `-Strict`锛氬瓨鍦?`FAIL` 鏃堕€€鍑?1锛屼粎瀛樺湪 `MISSING` 鏃堕€€鍑?2锛屼究浜庤嚜鍔ㄥ寲闂ㄧ銆?

6. **鏂囨。**  
   - `testdata/README.md` 琛ュ厖鍥炲綊鑴氭湰涓?`-Strict`銆乣-TestsOnly` 璇存槑銆?

### 楠岃瘉璁板綍

- 宸插湪鏈満鎵ц锛歚cmake -DPHOENIX_BUILD_GUI=OFF` 鈫?鏋勫缓 鈫?`ctest`锛宍UtilsNumeric` 閫氳繃锛沗scripts/Regress.ps1 -TestsOnly` 閫€鍑虹爜 0銆? 
- 瀹屾暣 GUI 鏋勫缓浠嶉渶鏈満閰嶇疆 `Qt5`锛坄CMAKE_PREFIX_PATH` / `-QtPrefix`锛夈€?

---

## LOG-20260423-001

| 瀛楁 | 鍐呭 |
|------|------|
| 鏃ユ湡 | 2026-04-23 |
| 绫诲瀷 | 娴嬭瘯鏁版嵁钀藉湴琛ラ綈锛圖FTT 瀹為檯闀滃儚鍏ュ簱 + 缁撴灉鍩虹嚎 + 鑷姩瀵规瘮鑴氭湰锛?|
| 鍏宠仈閲岀▼纰?| TECHNICAL_PLAN 搂5銆乀EST_EXECUTION_MATRIX |

### 鍙樻洿鎽樿

1. **DFTT 瑙ｅ帇浜х墿褰掓。鍒版爣鍑嗙洰褰?*  
   - 灏嗗凡瑙ｅ帇鏍锋湰缁熶竴褰掓。鍒?`testdata/dftt/`锛? 
     - `6-undel-fat/6-fat-undel.dd`  
     - `7-undel-ntfs/7-ntfs-undel.dd`  
     - `11-carve-fat/11-carve-fat.dd`  
     - `14-iso9660-1/*.iso`

2. **鐢熸垚棰勬湡鎭㈠缁撴灉鍩虹嚎**  
   - 鏂板 `testdata/manifests/expected_results.csv`銆? 
   - 鍩轰簬 DFTT `index.html` 鎻愬彇 FAT/NTFS Undelete 鍦烘櫙鐨勯鏈熸枃浠躲€佸ぇ灏忎笌 MD5銆? 
   - 瀵?NTFS `:ADS` 鏍囪 `include_in_auto_compare=false`锛岄伩鍏嶈法宸ュ叿鍛藉悕宸紓閫犳垚璇垽銆?

3. **鏂板鑷姩瀵规瘮鑴氭湰**  
   - 鏂板 `testdata/tools/Compare-RecoveryResults.ps1`銆? 
   - 杈撳叆鎭㈠杈撳嚭鏍圭洰褰曞悗锛岃嚜鍔ㄨ绠?MD5/澶у皬骞跺鍑?`compare_report.csv`锛坄PASS/FAIL/MISSING`锛夈€?

4. **娴嬭瘯鏁版嵁璇存槑鏂囨。鏇存柊**  
   - 鏇存柊 `testdata/README.md`锛岃ˉ鍏呭熀绾胯矾寰勫拰涓€閿瘮瀵瑰懡浠ょず渚嬨€?

### 楠岃瘉璁板綍

- 宸叉牎楠屾湰鍦伴暅鍍忔枃浠跺瓨鍦ㄥ苟璁＄畻 MD5锛? 
  - `6-fat-undel.dd` 鈫?`4aeb06ecd361777242ab78735d51ace6`  
  - `7-ntfs-undel.dd` 鈫?`e7dbb96759d9cd62b729463ebfe61dab`  
  - `11-carve-fat.dd` 鈫?`0069813c892a462f88dc6d376624f7d9`  
  - `iso-dirtree1.iso` 鈫?`481c16151a64ef7cb86bee07aab6115b`  
  - `iso-dirtree2.iso` 鈫?`a8653cf9568cab9826a7b812776d744a`  
  - `iso-endian.iso` 鈫?`76aa526d12ea82f7c317d268f291e1f5`

---

## LOG-20260422-001

| 瀛楁 | 鍐呭 |
|------|------|
| 鏃ユ湡 | 2026-04-22 |
| 绫诲瀷 | 娴嬭瘯鏁版嵁钀藉湴锛堣嚜鍔ㄤ笅杞?+ 鎵嬪姩琛ラ綈鏂规锛?|
| 鍏宠仈閲岀▼纰?| TECHNICAL_PLAN 搂5銆乀EST_DATA_SOURCES銆乀EST_EXECUTION_MATRIX |

### 鍙樻洿鎽樿

1. **鍒涘缓鏈湴娴嬭瘯鏁版嵁鐩綍缁撴瀯**  
   - 鏂板缓锛歚testdata/downloads/`銆乣testdata/dftt/`銆乣testdata/media/`銆乣testdata/manifests/`銆?

2. **鑷姩涓嬭浇鎴愬姛鏍锋湰**  
   - `xiph-poster_big.jpg`銆乣BigBuckBunny-surround.opus`銆乣sample-5s.mp4` 宸蹭笅杞藉苟鍒嗙被鍒?`testdata/media/`銆?

3. **DFTT 鑷姩涓嬭浇鍙楅檺璇存槑**  
   - 鍦ㄥ綋鍓嶅懡浠よ鐜涓嬶紝SourceForge 閾炬帴杩斿洖璺宠浆椤靛唴瀹癸紝瀵艰嚧 zip 涓嶅彲瑙ｅ帇锛堝凡閫氳繃 `tar -tf` 楠岃瘉澶辫触锛夈€? 
   - 鏂板 `testdata/README.md` 鎻愪緵鎵嬪姩涓嬭浇鍏ュ彛涓庝竴閿В鍘嬪懡浠ゃ€?

4. **鏍锋湰娓呭崟鐧昏**  
   - 鏂板 `testdata/manifests/datasets.csv`锛岃褰曟潵婧愩€佺姸鎬侊紙`downloaded` / `manual_required`锛変笌鐢ㄩ€斿娉ㄣ€?

---

## LOG-20260402-008

| 瀛楁 | 鍐呭 |
|------|------|
| 鏃ユ湡 | 2026-04-02 |
| 绫诲瀷 | 娴嬭瘯宸ョ▼鍖栵紙鏂板鎵ц鐭╅樀锛?|
| 鍏宠仈閲岀▼纰?| TECHNICAL_PLAN 搂5锛堟祴璇曚笌璐ㄩ噺淇濋殰锛?|

### 鍙樻洿鎽樿

1. **鏂板娴嬭瘯鎵ц鐭╅樀鏂囨。**  
   - 鏂板缓 `docs/TEST_EXECUTION_MATRIX.md`銆? 
   - 灏嗗姛鑳界偣涓庢暟鎹簮缁戝畾涓哄彲鎵ц鐢ㄤ緥锛堟壂鎻忋€佹仮澶嶃€侀瑙堛€侀洉鍒汇€佹姤鍛婏級銆?

2. **瀹氫箟鐗堟湰闂ㄧ**  
   - 缁欏嚭鍙戠増鍓?P0 蹇呰窇鐢ㄤ緥涓庨樆鏂潯浠讹紙鍚屽嵎淇濇姢銆佹仮澶嶅穿婧冦€佸彇娑堝け鏁堢瓑锛夈€?

3. **缁熶竴缁撴灉璁板綍妯℃澘**  
   - 澧炲姞 `run_id/build_version/case_id/dataset_id/result/evidence` 绛夊瓧娈靛缓璁紝渚夸簬鍚庣画鑷姩鍖栨帴鍏ャ€?

4. **鎶€鏈柟妗堟枃妗ｆ寕鎺?*  
   - 鍦?`docs/TECHNICAL_PLAN.md` 鐨勭浉鍏虫枃妗ｄ腑鏂板 `TEST_EXECUTION_MATRIX.md` 閾炬帴銆?

---

## LOG-20260402-007

| 瀛楁 | 鍐呭 |
|------|------|
| 鏃ユ湡 | 2026-04-02 |
| 绫诲瀷 | 娴嬭瘯璧勪骇寤鸿锛堝湪绾挎祴璇曟暟鎹簮鏀堕泦涓庢枃妗ｅ寲锛?|
| 鍏宠仈閲岀▼纰?| TECHNICAL_PLAN 搂5锛堟祴璇曚笌璐ㄩ噺淇濋殰锛?|

### 鍙樻洿鎽樿

1. **鏂板娴嬭瘯鏁版嵁婧愭竻鍗曟枃妗?*  
   - 鏂板缓 `docs/TEST_DATA_SOURCES.md`锛屾敹褰曞彲鍦ㄧ嚎鑾峰彇鐨勬祴璇曟暟鎹潵婧愩€? 
   - 瑕嗙洊锛欳FReDS銆丏FTT銆丏igital Corpora銆丏FRWS銆丼leuthkit test data銆丟ovdocs1銆乆iph銆丗Fmpeg FATE銆?

2. **鎸変紭鍏堢骇缁欏嚭棣栨壒鏍锋湰寤鸿**  
   - 鏂囨。涓畾涔?P0/P1 閲囬泦浼樺厛绾т笌鈥滄渶灏忓彲鐢ㄦ牱鏈寘锛圡VP锛夆€濄€? 
   - 鏄庣‘鍏堟湇鍔″綋鍓嶆仮澶嶄富閾捐矾鍥炲綊锛屽啀鎵╁睍澶嶆潅鍦烘櫙銆?

3. **琛ラ綈鏍锋湰娌荤悊瑙勮寖**  
   - 澧炲姞 `datasets.csv` 寤鸿瀛楁锛堟潵婧愩€佸搱甯屻€佽鍙€佺敤閫斻€佽矗浠讳汉锛夈€? 
   - 澧炲姞鐩綍缁勭粐寤鸿锛屼究浜庡悗缁嚜鍔ㄥ寲鍥炲綊涓庡悎瑙勫璁°€?

4. **鎶€鏈柟妗堟枃妗ｆ寕鎺?*  
   - 鍦?`docs/TECHNICAL_PLAN.md` 鐨勨€滅浉鍏虫枃妗ｂ€濅腑澧炲姞 `TEST_DATA_SOURCES.md` 閾炬帴銆?

---

## LOG-20260402-006

| 瀛楁 | 鍐呭 |
|------|------|
| 鏃ユ湡 | 2026-04-02 |
| 绫诲瀷 | 闃舵 2 棣栨锛堢粺涓€鎵弿闃舵鏂囨湰閫忎紶鍒?UI锛?|
| 鍏宠仈閲岀▼纰?| TECHNICAL_PLAN 搂6 闃舵 2锛堢粺涓€鎵弿娴佹按瀹屽杽 / 闃舵杩涘害鍙锛?|

### 鍙樻洿鎽樿

1. **鎵弿杩涘害缁撴瀯澧炲姞闃舵鏂囨湰**  
   - `scan/ScanEngine.h` 鐨?`ScanProgress` 鏂板 `stageLabel`锛坄std::wstring`锛夈€?

2. **缁熶竴鎵弿鍏ュ彛琛ュ厖闃舵鍖栬繘搴?*  
   - `scan/UnifiedVolumeScanner.cpp` 鍦ㄧ粺涓€鍏ュ彛娉ㄥ叆闃舵鏍囩锛? 
     - 鍚姩锛歚闃舵 1/1 鍏冩暟鎹В鏋恅  
     - 杩愯涓細瀵瑰簳灞傝繘搴﹁ˉ榻愰粯璁ら樁娈垫爣绛? 
     - 瀹屾垚锛歚闃舵 1/1 鍏冩暟鎹В鏋愶紙瀹屾垚锛塦

3. **Controller/Worker/UI 鍏ㄩ摼璺€忎紶闃舵鏂囨湰**  
   - `RecoveryScanWorker::progress` 淇″彿鏂板 `phaseText` 鍙傛暟锛屽苟浠?`ScanProgress.stageLabel` 杞负 `QString` 鍙戝嚭銆? 
   - `RecoveryScanController::scanProgress` 淇″彿绛惧悕鍚屾銆? 
   - `RecoveryScanPage::setScanProgress(...)` 鏂板 `phaseText` 鍙傛暟骞舵樉绀轰负锛歚鎵弿杩涜涓?路 <闃舵鏂囨湰>`銆?

### 璇存槑

- 褰撳墠缁熶竴鎵弿浠嶄负鍗曚竴鍏ュ彛锛岄樁娈垫枃鏈敤浜庡厛寤虹珛 UI 鍙娴嬭兘鍔涳紱鍚庣画鎺ュ叆鏇村鍐呴儴闃舵锛堟墿灞曟壂鎻?绛惧悕闆曞埢锛夋椂鍙洿鎺ュ鐢ㄨ閾捐矾銆? 

---

## LOG-20260402-005

| 瀛楁 | 鍐呭 |
|------|------|
| 鏃ユ湡 | 2026-04-02 |
| 绫诲瀷 | 闃舵 1 灞曠ず灞傚崌绾э紙鎭㈠瀹屾垚椤佃鍥炬浛浠ｇ函寮瑰眰锛?|
| 鍏宠仈閲岀▼纰?| TECHNICAL_PLAN 搂6 闃舵 1锛?.3 鎭㈠瀹屾垚椤?鎽樿锛?|

### 鍙樻洿鎽樿

1. **鎵弿椤靛唴鏂板鈥滄仮澶嶅畬鎴愯鍥锯€?*锛坄pages/RecoveryScanPage.*`锛? 
   - 鏂板 `recoveryCompletePanel_` 瑕嗙洊灞傦紙缁熻鎽樿 + 鐩爣鐩綍 + 鏄庣粏琛?+ 鎿嶄綔鎸夐挳锛夈€? 
   - 鎭㈠瀹屾垚鍚庣敱 `showRecoveryCompletePanel(...)` 灞曠ず锛屾敮鎸佲€滆繑鍥炴壂鎻忕粨鏋溾€濆叧闂€? 
   - 鏂板 `resizeEvent(...)`锛岀獥鍙ｅ彉鍖栨椂鑷姩閲嶆帓瀹屾垚瑙嗗浘銆?

2. **鎭㈠缁撴灉缁撴瀯鍖栧睍绀?*锛坄pages/RecoveryScanPage.cpp`锛? 
   - 灏?`RecoverBatchResult.details` 娓叉煋涓鸿〃鏍硷紙鐘舵€?鏂囦欢/鐩爣璺緞/鍘熷洜锛夈€? 
   - 淇濈暀鎭㈠瀹屾垚淇℃伅鎻愮ず锛岄伩鍏嶇敤鎴烽敊杩囨垚鍔熷弽棣堛€?

3. **瀹屾垚瑙嗗浘鎿嶄綔鎸夐挳**锛坄pages/RecoveryScanPage.cpp`锛? 
   - 鈥滄墦寮€鐩綍鈥濓細璋冪敤 `QDesktopServices::openUrl(...)` 鎵撳紑鎭㈠鐩爣鐩綍銆? 
   - 鈥滃鍑烘槑缁?..鈥濇敼涓?`exportRecoveryDetailReport()`锛屽鐢ㄤ笂娆℃仮澶嶆槑缁嗕笌鐩爣鐩綍銆? 
   - 鈥滆繑鍥炴壂鎻忕粨鏋溾€濓細闅愯棌瀹屾垚瑙嗗浘锛屼笉涓柇宸叉湁鎵弿缁撴灉娴忚鑳藉姏銆?

### 璇存槑

- 瀹屾垚椤典互鎵弿椤佃鐩栧眰瀹炵幇锛岄伩鍏嶆敼鍔ㄤ富绐楀彛璺敱缁撴瀯锛涘悗缁闇€鐙珛璺敱椤甸潰锛屽彲鍦ㄤ笉鍙樻洿鎭㈠寮曟搸閫昏緫鐨勫墠鎻愪笅鏇挎崲 UI 瀹瑰櫒銆? 

---

## LOG-20260402-004

| 瀛楁 | 鍐呭 |
|------|------|
| 鏃ユ湡 | 2026-04-02 |
| 绫诲瀷 | 闃舵 1 浣撻獙琛ラ綈锛堟仮澶嶆槑缁嗗脊灞?+ 瀵煎嚭 + 淇濈暀鐩綍缁撴瀯閫夐」锛?|
| 鍏宠仈閲岀▼纰?| TECHNICAL_PLAN 搂6 闃舵 1锛?.2 / 1.3锛?|

### 鍙樻洿鎽樿

1. **鎭㈠鍙傛暟琛ラ綈**锛坄pages/RecoveryScanPage.*`锛? 
   - Footer 鏂板 `keepDirStructureCheck_`锛堥粯璁ゅ嬀閫夆€滀繚鐣欏師鐩綍缁撴瀯鈥濓級銆? 
   - `RecoverBatch(...)` 涓嶅啀纭紪鐮?`keepDirStructure=true`锛屾敼涓鸿鍙栧閫夋鐘舵€併€?

2. **鎭㈠缁撴灉寮瑰眰涓庡け璐ユ槑缁?*锛坄pages/RecoveryScanPage.cpp`锛? 
   - 鎭㈠瀹屾垚鍚庣敱绠€鍗曟彁绀哄崌绾т负缁撴灉寮瑰眰锛氭垚鍔?澶辫触/璺宠繃/鏄惁鍙栨秷銆? 
   - 鐢熸垚姣忔潯鏂囦欢鎭㈠璁板綍锛堢姸鎬併€佹枃浠跺悕銆佺洰鏍囪矾寰勩€佸師鍥狅級骞跺啓鍏?`detailedText`銆?

3. **澶辫触鏄庣粏瀵煎嚭**锛坄pages/RecoveryScanPage.cpp`锛? 
   - 鍦ㄧ粨鏋滃脊灞傚鍔犫€滃鍑烘槑缁?..鈥濇寜閽€? 
   - 鏀寔灏嗘槑缁嗗鍑轰负 UTF-8 BOM 鏂囨湰锛堥粯璁ゅ缓璁?`recover_report.txt`锛夈€?

### 璇存槑

- 褰撳墠鈥滄仮澶嶅畬鎴愰〉鈥濋噰鐢ㄧ粨鏋滃脊灞傚疄鐜帮紝鍏堟弧瓒抽棴鐜笌璇婃柇鑳藉姏锛涜嫢鍚庣画闇€瑕佺嫭绔嬮〉闈紝鍙湪涓嶆敼搴曞眰鎭㈠娴佺▼鐨勬儏鍐典笅鏇挎崲灞曠ず灞傘€? 

---

## LOG-20260402-003

| 瀛楁 | 鍐呭 |
|------|------|
| 鏃ユ湡 | 2026-04-02 |
| 绫诲瀷 | 闃舵 1 瀹炵幇锛堟仮澶嶉棴鐜?v1锛氱洰褰曢€夋嫨 + 鍚屽嵎鏍￠獙 + 鎵归噺鎭㈠锛?|
| 鍏宠仈閲岀▼纰?| TECHNICAL_PLAN 搂6 闃舵 1锛?.1 / 1.2锛?|

### 鍙樻洿鎽樿

1. **鎭㈠鎸夐挳鎺ラ€氱湡瀹炴祦绋嬶紙`pages/RecoveryScanPage.cpp`锛?*  
   - `recoverAllBtn_` 鏂板鐐瑰嚮澶勭悊 `onRecoverAllClicked()`銆? 
   - 浠庤〃鏍煎嬀閫夋鏀堕泦鍙仮澶嶆枃浠?ID锛堣嚜鍔ㄨ烦杩囩洰褰曪級銆? 
   - 寮瑰嚭鐩綍閫夋嫨妗嗭紝鎵ц `RecoverEngine::ValidateTargetPath(...)` 鍚屽嵎鏍￠獙銆? 
   - 鎵撳紑婧愬嵎鍚庤皟鐢?`RecoverEngine::RecoverBatch(...)`锛屽苟浣跨敤 `QProgressDialog` 灞曠ず杩涘害銆佹敮鎸佸彇娑堛€? 
   - 鎭㈠缁撴潫寮圭獥灞曠ず鎴愬姛/澶辫触/璺宠繃/鍙栨秷缁熻銆?

2. **鎵弿缁撴灉瀛樺偍琛ラ綈鎭㈠鎵€闇€ `FileNode`锛坄controllers/ScanResultStore.*`锛?*  
   - 鏂板 `setRecoveredFileNode(...)` / `tryGetRecoveredFileNode(...)`銆? 
   - `clear()` 鏃跺悓姝ユ竻鐞?`recoveredFileNodes_`銆? 
   - 浣滅敤锛氭仮澶嶆祦绋嬩笉鍐嶄緷璧栭瑙堢紦瀛橈紝鑳芥嬁鍒板畬鏁村簳灞傛仮澶嶅厓鏁版嵁銆?

3. **Worker 鍦ㄦ壂鎻忔椂淇濆瓨搴曞眰鑺傜偣锛坄controllers/RecoveryScanWorker.cpp`锛?*  
   - 鏂囦欢鍥炶皟鍒拌揪鏃讹紝鎶?`FileNode` 蹇収鍐欏叆 `ScanResultStore`銆? 
   - 涓庡凡鏈?`ScanFileDto` 骞惰淇濆瓨锛孶I 缁х画杞婚噺灞曠ず锛屾仮澶嶈蛋瀹屾暣鏁版嵁銆?

### 璇存槑

- 褰撳墠鎭㈠鍏ュ彛鏄€滄仮澶嶅叏閮紙鎸夊嬀閫夛級鈥濓紝宸插叿澶囬樁娈?1 鐨勫熀纭€闂幆銆? 
- 鎭㈠瀹屾垚椤?鏇寸粏绮掑害澶辫触鏄庣粏 UI 浠嶅彲鍦ㄥ悗缁凯浠ｈˉ榻愩€? 

---

## LOG-20260402-002

| 瀛楁 | 鍐呭 |
|------|------|
| 鏃ユ湡 | 2026-04-02 |
| 绫诲瀷 | 鏋舵瀯鏀舵暃淇锛堟寜浜у搧瑕佹眰鍥炴敹鎵弿妯″紡锛屾敼涓哄崟涓€鎵弿娴佺▼锛?|
| 鍏宠仈閲岀▼纰?| TECHNICAL_PLAN 搂6 闃舵 0锛堝崟鍏ュ彛鎵弿銆佸彇娑堥摼璺€佺粺涓€鍏ュ彛锛?|

### 鍙樻洿鎽樿

1. **鎸夐渶姹傜Щ闄も€滃揩鎵?娣辨壂/RAW 妯″紡鍒囨崲鈥?*  
   - 鍒犻櫎 `RecoveryScanMode`锛坄controllers/ScanTypes.h`锛夈€? 
   - `RecoveryScanController::startVolumeScan(...)` 鎭㈠涓?*浠呯洏绗﹀弬鏁?*锛屽苟鏀瑰洖璋冪敤 Worker 鐨?`runVolumeScan`銆?

2. **`RecoveryScanWorker` 鏀舵暃涓哄崟涓€璺緞**  
   - 鍒犻櫎 `runVolumeScanWithMode(...)` 鍙婃ā寮忚浆鎹㈤€昏緫銆? 
   - 缁熶竴浣跨敤 `UnifiedVolumeScanner` 浣滀负鍞竴楂樺眰鎵弿鍏ュ彛銆? 
   - 淇濈暀骞舵部鐢ㄥ凡鎺ュ叆鐨?`SectorReader::SetCancelFlag(&cancelFlag_)`锛岀‘淇濆彇娑堥摼璺户缁敓鏁堛€?

3. **鎶€鏈柟妗堟枃妗ｅ悓姝ワ紙`docs/TECHNICAL_PLAN.md`锛?*  
   - 灏嗏€滄壂鎻忔ā寮忚矾鐢扁€濊皟鏁翠负鈥滅粺涓€鎵弿娴佹按锛堝唴閮ㄩ樁娈碉級鈥濄€? 
   - 闃舵 2 鏀逛负鈥滅粺涓€鎵弿娴佹按瀹屽杽鈥濓紝涓嶅啀瑕佹眰 UI 鏆撮湶妯″紡閫夋嫨銆? 
   - 鎬ц兘鍩哄噯鏀逛负缁熶竴娴佹按鍒嗛樁娈佃鏃躲€?

### 璇存槑

- 鏈涓?*闇€姹傚榻愪慨澶?*锛氫繚鎸佸唴閮ㄥ彲鎵╁睍鑳藉姏锛團S 鍏冩暟鎹€佺鍚嶉洉鍒荤瓑闃舵锛変絾涓嶅啀浣滀负鐢ㄦ埛鍙€夋ā寮忔毚闇层€? 

### 楠岃瘉璁板綍

- 宸叉墽琛岋細`cmake -S ... -B .../build`锛圵indows VS2019 鐢熸垚鍣級銆? 
- 缁撴灉锛氶厤缃け璐ワ紝鍘熷洜涓烘湰鏈烘湭閰嶇疆 `Qt5Config.cmake`锛坄find_package(Qt5 5.14 REQUIRED ...)` 鏃犳硶瑙ｆ瀽锛夛紱灞炰簬鐜渚濊禆闂锛岄潪鏈浠ｇ爜淇敼寮曞叆銆? 

---

## LOG-20260402-001

| 瀛楁 | 鍐呭 |
|------|------|
| 鏃ユ湡 | 2026-04-02 |
| 绫诲瀷 | 鍔熻兘 / 鏋舵瀯锛堥樁娈?0锛氭壂鎻忔ā寮忚矾鐢?+ 鍙栨秷涓?I/O锛?|
| 鍏宠仈閲岀▼纰?| TECHNICAL_PLAN 搂6 闃舵 0锛圫canMode銆佸彇娑堛€佺粺涓€寮曟搸鍏ュ彛銆佹€ц兘鍙鎬ч儴鍒嗭級 |

### 鍙樻洿鎽樿

1. **`RecoveryScanMode`锛坄controllers/ScanTypes.h`锛?*  
   - 鏂板 `Quick` / `Deep` / `Raw`锛屼笌 PRD 涓揩閫?娣卞害/RAW 鎵弿璇箟瀵归綈銆? 
   - `Q_DECLARE_METATYPE(RecoveryScanMode)`锛屼緵鎺у埗灞傛敞鍐屼笌鍚庣画 UI 鎵╁睍銆?

2. **`RecoveryScanWorker`锛坄controllers/RecoveryScanWorker.cpp`锛?*  
   - `runVolumeScan` 濮旀墭涓?`runVolumeScanWithMode(drive, int)`锛岄粯璁よ涓轰笉鍙橈紙Quick锛夈€? 
   - 鎸夋ā寮忓垱寤?`QuickScan` / `DeepScan` / `RawScan`锛堝潎缁ф壙 `ScanEngine`锛夛紝鏇夸唬鍘熷厛鐩存帴瀹炰緥鍖?`VolumeSectorScanner`锛決uick 浠嶉€氳繃 `QuickScan` 鈫?`VolumeSectorScanner`锛屼笌 `UnifiedVolumeScanner` 鍐呴儴璺緞涓€鑷淬€? 
   - Quick锛歚SetExternalCancelFlag(&cancelFlag_)`锛汥eep/Raw锛歚SetCancelCallback` 璇诲彇鍚屼竴 `cancelFlag_`銆?

3. **`RecoveryScanController`锛坄.h` / `.cpp`锛?*  
   - `startVolumeScan(drive, RecoveryScanMode mode = Quick)`锛屽唴閮?`invokeMethod(..., "runVolumeScanWithMode", Q_ARG(QString,...), Q_ARG(int,...))`銆? 
   - 鏋勯€犱腑 `qRegisterMetaType<RecoveryScanMode>`銆? 
   - 鐜版湁浠呬紶鐩樼鐨勮皟鐢?*鏃犻渶淇敼**锛岄粯璁?Quick銆?

4. **`SectorReader`锛坄disk/SectorReader.h` / `.cpp`锛?*  
   - `SetCancelFlag(std::atomic<bool>*)`锛沗ReadSectors` 鍦ㄦ暣娈典笌鎸夋墖鍖烘參璺緞鐨勫惊鐜腑妫€鏌ュ彇娑堬紝`m_lastWinError = ERROR_OPERATION_ABORTED` 鍚庤繑鍥?`false`锛屼究浜庤В鏋愬眰鎻愬墠缁撴潫銆? 
   - `Close()` 娓呯┖ `m_pCancel` 閬垮厤鎮┖銆? 
   - Worker 鍦?`OpenVolume` 鎴愬姛鍚庡褰撳墠 `reader` 璁剧疆 `&cancelFlag_`銆?

### 鏈湪姝ょ幆澧冩墽琛岄」

- 鏈満 PowerShell 涓?`msbuild` 涓嶅湪 `PATH`锛?*鏈?*瀹屾垚 MSVC 瀹炵紪璇戯紱璇峰湪 Visual Studio 寮€鍙戣€呭懡浠よ鎴栧凡閰嶇疆 PATH 鐨勭幆澧冧腑瀵?`PhoenixRecover.sln` 鎵ц鐢熸垚浠ヤ綔楠岃瘉銆?

### 鍚庣画寤鸿

- 鍦?`RecoveryLocationPage`锛堟垨鎵弿鍏ュ彛锛夊皢 UI 鐨勩€屾壂鎻忔ā寮忋€嶇粦瀹氬埌 `startVolumeScan(..., mode)`銆? 
- 涓?`ReadSectors` 鍦ㄥ彇娑堣矾寰勮ˉ鍏呭崟鍏冩祴璇曟垨闀滃儚闆嗘垚娴嬭瘯锛堥渶鍙繍琛?I/O 妗╋級銆? 

---

## 鎵弿缁撴灉 UI 杩佺Щ锛歈TableWidget 鈫?QTableView + Model/View锛?026-05-26锛?

### 鑳屾櫙

`RecoveryScanPage` 鍘熶娇鐢?`QTableWidget`锛堥」鎺т欢妯″紡锛夊睍绀烘壂鎻忕粨鏋滐紝鍦ㄥぇ鏁版嵁闆嗭紙>1 涓囨潯锛夋椂瀛樺湪鍐呭瓨鏆存定鍜?UI 鍗￠】闂銆傛寜鐓?`docs/DESIGN_LargeResultUI.md` 涓殑鏂规锛岃縼绉诲埌 `QAbstractTableModel` + `QSortFilterProxyModel` + `QTableView` 鏋舵瀯銆?

### 鍙樻洿娓呭崟

1. **`controllers/ScanResultStore.h/.cpp`**
   - 鏂板 `insertOrder_` 鎴愬憳锛坄QVector<quint64>`锛夛紝鍦?`upsert()` 涓淮鎶ゆ彃鍏ラ『搴忋€?
   - 鏂板 `filesByInsertOrder(int offset, int count)` 鏂规硶锛屾敮鎸?Model 澧為噺鍚屾銆?
   - `clear()` 涓悓姝ユ竻绌?`insertOrder_`銆?

2. **鏂板缓 `models/ScanResultModel.h/.cpp`**锛堢户鎵?`QAbstractTableModel`锛?
   - 鍒楀畾涔夛細鍕鹃€夛紙CheckBox锛夈€佹枃浠跺悕銆佸ぇ灏忋€佺被鍨?鎵╁睍鍚嶃€佷慨鏀规椂闂淬€佸彲鎭㈠鎬с€佽矾寰勩€佸彂鐜伴樁娈点€?
   - `appendBatch(batch)` 浣跨敤 `beginInsertRows/endInsertRows` 鎵归噺杩藉姞銆?
   - `setData()` 鏀寔 `Qt::CheckStateRole` 鐢ㄤ簬鍕鹃€夊垏鎹€?
   - `data()` 涓负鍙仮澶嶆€у垪杩斿洖 `Qt::ForegroundRole` 棰滆壊瑁呴グ锛堟甯?缁裤€佸凡閲嶅缓=姗欍€佸凡鍒犻櫎=绾級銆?
   - `flags()` 涓哄嬀閫夊垪杩斿洖 `Qt::ItemIsUserCheckable`銆?
   - `onStoreRevisionChanged(revision)` 浠?`ScanResultStore` 澧為噺鍚屾銆?
   - `checkedIds()` / `setAllChecked()` / `checkedCount()` 绠＄悊鍕鹃€夌姸鎬併€?

3. **鏂板缓 `models/ScanResultFilterModel.h/.cpp`**锛堢户鎵?`QSortFilterProxyModel`锛?
   - `setNameFilter(keyword)` 鈥?鏂囦欢鍚嶆ā绯婂尮閰嶃€?
   - `setExtensionFilter(exts)` 鈥?鎵╁睍鍚嶇瓫閫夈€?
   - `setSizeRange(min, max)` 鈥?澶у皬鑼冨洿銆?
   - `setDateRange(from, to)` 鈥?鏃ユ湡鑼冨洿銆?
   - `setRecoverabilityFilter(minLevel)` 鈥?鏈€浣庡彲鎭㈠鎬с€?
   - `setPathPrefix(relPath)` 鈥?璺緞鍓嶇紑锛堜笌璺緞鏍戣仈鍔級銆?
   - `setTypeCategory(category)` 鈥?绫诲瀷鍒嗙被锛堜笌绫诲瀷鏍戣仈鍔級銆?
   - `filterAcceptsRow()` 瀹炵幇缁勫悎杩囨护銆?
   - `lessThan()` 涓哄ぇ灏忓垪鍜屾椂闂村垪鎻愪緵姝ｇ‘鎺掑簭銆?

4. **淇敼 `pages/RecoveryScanPage.h`**
   - 鏂板鎴愬憳锛歚QTableView *tableView_`銆乣ScanResultModel *scanModel_`銆乣ScanResultFilterModel *filterModel_`銆乣QTimer *searchTimer_`銆?
   - 淇濈暀 `table_`锛堢疆涓?`nullptr`锛屽悗缁彲瀹屽叏绉婚櫎锛夈€?

5. **淇敼 `pages/RecoveryScanPage.cpp`**锛堢害 25 澶勫眬閮ㄦ浛鎹紝鏈叏閲忛噸鍐欙級
   - 鏋勯€犲嚱鏁颁腑鐢?`QTableView` + Model + FilterModel 鏇夸唬 `QTableWidget` 鐨勫垱寤轰笌閰嶇疆銆?
   - `tableView_` 鍚敤 `setSortingEnabled(true)`锛屾敮鎸佺偣鍑昏〃澶存帓搴忋€?
   - 鎼滅储妗嗚繛鎺ユ敼涓?300ms QTimer debounce 鈫?`filterModel_->setNameFilter()`銆?
   - `headerSelectAllCheckBox_` 鐨勫叏閫?鍙栨秷鏀逛负璋冪敤 `scanModel_->setAllChecked()`銆?
   - `checkedRecoverableIdsFromTable()` 鏀逛负璋冪敤 `scanModel_->checkedIds()`銆?
   - `updateHeaderSelectAllCheckState()` 鏀逛负浠?`scanModel_->checkedCount()` 璇诲彇銆?
   - `syncTableChecksFromTreeSelection()` 鏀逛负閬嶅巻 `scanModel_->dtoAtRow()` + `setChecked()`銆?
   - `refillTableFromPathItem()` 澧炲姞 `filterModel_->setPathPrefix()` 璋冪敤銆?
   - `appendTableRow()` 缃负绌哄嚱鏁帮紙QTableView 鐢?Model 鑷姩椹卞姩锛夈€?
   - `tableContainsDto()` / `tableRowCheckBoxAt()` 绠€鍖栦负鍏煎妗┿€?
   - `onScanIndexUpdated()` 涓柊澧?`scanModel_->onStoreRevisionChanged()` 璋冪敤銆?
   - `appendScanFileBatch()` 涓柊澧?`scanModel_->appendBatch()` 璋冪敤銆?
   - 鏍峰紡琛ㄤ腑琛ュ厖 `QTableView` 閫夋嫨鍣ㄤ互鍖归厤鏂版帶浠舵牱寮忋€?

6. **`CMakeLists.txt`**
   - `PHOENIX_SOURCES` 涓坊鍔?`models/ScanResultModel.cpp` 鍜?`models/ScanResultFilterModel.cpp`銆?

### 鍏煎鎬?

- 璺緞鏍戝拰绫诲瀷鏍戠殑閫夋嫨浠嶇劧閫氳繃 FilterModel 杩囨护琛ㄦ牸鍐呭銆?
- 鎭㈠娴佺▼浠嶇劧閫氳繃 `checkedRecoverableIdsFromTable()` 鈫?`scanModel_->checkedIds()` 鑾峰彇鍕鹃€夋枃浠?ID銆?
- Grid锛堥瑙堬級瑙嗗浘鏆傛湭杩佺Щ锛屼粛浣跨敤 `QListWidget`锛屼笌 Model 骞惰杩愯銆?
- `createTableCheckBoxWidget()` 淇濈暀浣嗕笉鍐嶈 QTableView 浣跨敤锛堜粎鏈嶅姟浜?Grid 鍗＄墖鐨?checkbox锛夈€?

### 鍚庣画寤鸿

- 闃舵 4锛氬皢 Grid 棰勮瑙嗗浘浠?`QListWidget` 杩佺Щ鍒?`QListView` + `QStyledItemDelegate` + `ThumbnailCache`銆?
- 闃舵 5锛氱Щ闄?`QTableWidget` 鐩稿叧鐨勬畫鐣欎唬鐮侊紙`table_` 鎴愬憳銆乣appendTableRow()`銆乣tableRowCheckBoxAt()` 绛夛級銆?
- 鎬ц兘鍩哄噯娴嬭瘯锛氶獙璇?10 涓囨潯璁板綍鍦烘櫙涓嬬殑婊氬姩甯х巼銆佹悳绱㈠搷搴旀椂闂淬€佸唴瀛樺崰鐢ㄣ€?

---

## LOG-20260526-008: P4 鍗曞厓娴嬭瘯淇 + P5 娴嬭瘯鏁版嵁琛ュ厖

**鏃ユ湡**: 2026-05-26  
**浠诲姟**: 瀹屾垚 P4 鍏ㄦā鍧楀崟鍏冩祴璇曢獙鏀?+ P5 娴嬭瘯鏁版嵁琛ュ厖

### P4 鍗曞厓娴嬭瘯

宸叉湁 6 涓崟鍏冩祴璇?+ 2 涓泦鎴愭祴璇曞叏閮ㄧ紪璇戦€氳繃锛?

| 娴嬭瘯鍚嶇О | 妯″潡 | 鐘舵€?|
|----------|------|------|
| UtilsNumeric | common/Utils | PASS |
| FatStructs | fs/fat | PASS |
| NtfsStructs | fs/ntfs | PASS (淇 DataRun header 0x21鈫?x22) |
| CarveCommon | scan/Carve | PASS |
| ScanProgress | scan/ScanEngine | PASS |

**淇椤?*: `test_ntfs_structs.cpp` 娴嬭瘯鐢ㄤ緥 2g 鐨?DataRun header 閿欒鈥斺€擿0x21` (lenFieldSize=1, offFieldSize=2) 琚敊璇敤浜?2 瀛楄妭闀垮害瀛楁鐨勫満鏅紝姝ｇ‘鍊间负 `0x22` (lenFieldSize=2, offFieldSize=2)銆?

### P5 娴嬭瘯鏁版嵁琛ュ厖

1. **淇鏍锋湰鍒涘缓** (`testdata/repair_samples/`):
   - `truncated.jpg` 鈥?JPEG 鎴柇鑷?80%锛?19,657 瀛楄妭锛?
   - `corrupted_ftyp.mp4` 鈥?ftyp box 鍓?瀛楄妭娓呴浂锛?,848,208 瀛楄妭锛?
   - `truncated.zip` 鈥?鏈熬鎴幓 100 瀛楄妭锛孍OCD 鎹熷潖锛?92,406 瀛楄妭锛?
   - `normal.zip` / `original.mp4` 鈥?瀹屾暣瀵圭収鍩哄噯

2. **娓呭崟鏇存柊**:
   - 鏂板 `testdata/manifests/repair_samples.csv`锛? 鏉¤褰曪紝鍚?MD5銆佹崯鍧忕被鍨嬶級
   - `testdata/manifests/datasets.csv` 杩藉姞 REPAIR-SAMPLES 鏉＄洰
   - `testdata/README.md` 澧炲姞淇鏍锋湰浣跨敤璇存槑

---

## LOG-20260527-009: 娴嬭瘯鏁版嵁澶ц妯¤ˉ鍏?

**鏃ユ湡**: 2026-05-27  
**浠诲姟**: 浠?NIST CFReDS銆丏FTT銆丏igital Corpora 涓嬭浇鏇村娴嬭瘯闀滃儚

### 鏂板鏁版嵁婧?

| 鏉ユ簮 | 闀滃儚鏁?| 鍚堣澶у皬 | 鏂囦欢绯荤粺瑕嗙洊 |
|------|--------|----------|-------------|
| DFTT #8 JPEG Search | 1 | 10 MB | FAT (JPEG 鎼滅储鍦烘櫙) |
| DFTT #10 NTFS Autodetect | 3 | 94 MB | NTFS (鍚垎鍖鸿嚜鍔ㄦ娴? |
| NIST CFReDS DFR-01/02/03/05 | 18 | ~16 GB | FAT12/16/32, NTFS, Ext2/3/4, exFAT |
| Digital Corpora TSK-2024 | 1 ZIP (40+ 鏂囦欢) | 17 MB | FAT, NTFS, exFAT, BTRFS, XFS, APFS |

### CFReDS 闀滃儚璇存槑

- **DFR-01**: 鍩虹鍗曟枃浠跺垹闄ゆ仮澶嶏紙姣忎釜鏂囦欢绯荤粺绫诲瀷 1 涓垎鍖猴級
- **DFR-02**: 澶氭枃浠跺垹闄わ紙鏇村鏉傜殑鏂囦欢甯冨眬锛?
- **DFR-03**: 瑕嗙洊鍦烘櫙锛堥儴鍒嗙皣宸茶鏂版枃浠惰鍐欙級
- **DFR-05**: 纰庣墖鍖栨枃浠舵仮澶嶏紙鍚?braid 浜ょ粐纰庣墖鍙樹綋锛?
- 姣忕粍鍚?FAT/NTFS/Ext/exFAT 鍥涚鏂囦欢绯荤粺锛屽叡 18 涓?dd 闀滃儚
- bz2 鍘嬬缉鏂囦欢閫氳繃 Python bz2 妯″潡瑙ｅ帇锛圵indows tar 涓嶆敮鎸?bzip2锛?

### 涓嶅彲鐢ㄩ暅鍍?

- DFTT #9 (FAT Volume Label), #12 (Basic Carving #2): SourceForge 椤甸潰杩斿洖 404锛屽凡浠庤鍒掍腑绉婚櫎
- Digital Corpora 澶у瀷闀滃儚: 鍧囦负 E01 鏍煎紡锛屽綋鍓嶈В鏋愬櫒涓嶆敮鎸侊紝鏆備笉绾冲叆

### 娓呭崟鏇存柊

- `testdata/manifests/datasets.csv`: 浠?9 鏉″鑷?31 鏉?
- `testdata/README.md`: 鏇存柊鏁版嵁娓呭崟鍜屼娇鐢ㄨ鏄?

---

## LOG-20260527-012: 鎵╁睍銆屽叾浠栥€嶇被鍨嬫爲锛堥偖浠?涔︾/鏈繚瀛?鍏朵粬锛?

**鏃ユ湡**: 2026-05-27  

- `UiFileTypeCatalog` 鏂板鍥涚被锛?*閭欢**锛?锛夈€?*涔︾**锛?锛夈€?*鏈繚瀛樼殑鏂囦欢**锛?锛夈€?*鍏朵粬**锛?8锛?
- `CategoryLookupOrder` 鎺у埗澶氱被鍒墿灞曞悕褰掑睘锛堝 `tmp`鈫掓湭淇濆瓨銆乣webloc`鈫掍功绛撅級
- 琛ュ厖绛惧悕锛歚webloc`/`url`/`ini`/`inf`/`lnk`/`asd`/`ps1`/`cmd` 鍙?`ost`/`drv` 绛夊埆鍚?
- `ExtensionCoverage` 澧炲姞閭欢/涔︾/鏈繚瀛樺垽瀹氳鍒?
- `RecoveryScanPage` 瀹屽叏濮旀墭 catalog锛岀Щ闄ら噸澶?CSV 鍥為€€閫昏緫

---

## LOG-20260527-011: UI 绫诲瀷鏍戝瓙鎵╁睍鍚嶅叏瑕嗙洊锛堢鍚?+ 瑕嗙洊鐜囨祴璇曪級

**鏃ユ湡**: 2026-05-27  
**浠诲姟**: 淇濊瘉鍥剧墖/瑙嗛/鏂囨。/闊抽/鍘嬬缉鏂囦欢浜旂被涓嬫墍鏈夊瓙绫诲瀷鍧囨湁鏄庣‘鏀寔绛栫暐

### 鍙樻洿

- 鏂板 `common/UiFileTypeCatalog`锛歎I 绫诲瀷鏍戞墿灞曞悕鍗曚竴鏁版嵁婧愶紙298 涓墿灞曞悕锛?
- 鏂板 `scan/UiTypeSignatureSupplement`锛氳ˉ鍏呯鍚嶄笌鍒悕锛圫ignatureDB 鐜?494 鏉¤鍒欙級
- 鏂板 `scan/ExtensionCoverage` + CTest `UiTypeCoverage`锛欳I 寮哄埗鏍￠獙浜旂被瀛愮被鍨嬪潎鍙綊绫讳负銆岀洿鎺ョ鍚?/ 鍒悕 / ZIP路CFB 瀹瑰櫒 / 鍏冩暟鎹壂鎻忋€?
- `RecoveryScanPage` 绫诲瀷鍒嗙被涓庡瓙鑺傜偣鍒楄〃鏀逛负璇诲彇 `UiFileTypeCatalog`
- `scripts/check_type_coverage.py`锛氭湰鍦板樊璺濆垎鏋愯剼鏈?

### 鏀寔绛栫暐璇存槑

| 绛栫暐 | 鍚箟 |
|------|------|
| 鐩存帴绛惧悕 / 鍒悕 | 娣卞害鎵弿锛圧AW 闆曞埢锛夊彲璇嗗埆 |
| ZIP/CFB 瀹瑰櫒 | 鎸夊鍣ㄩ瓟鏁拌瘑鍒紙濡?docx鈫抸ip锛?|
| 浠呭厓鏁版嵁 | 鏃犲彲闈犻瓟鏁帮紙濡?txt/csv/cpp锛夛紱渚濊禆 NTFS/FAT 鍏冩暟鎹壂鎻忥紝闆曞埢涓嶄繚璇佸嚭鐜?|

鎭㈠寮曟搸浠嶅鎵€鏈夊凡鍙戠幇鏉＄洰鎸夌皣閾?鍋忕Щ鍐欑洏锛屼笌鎵╁睍鍚嶆棤鍏炽€?

---

## LOG-20260527-010: 鑷祴寮哄寲 鈥?Platform.h 鍘?Qt 渚濊禆 + 鏂板闆嗘垚娴嬭瘯

**鏃ユ湡**: 2026-05-27  
**浠诲姟**: 娑堥櫎闈?GUI 浠ｇ爜鐨?Qt 渚濊禆锛屾墿灞曢泦鎴愭祴璇曡鐩?

### Platform.h 閲嶆瀯

灏?`common/Platform.h` 浠?`#include <QtGlobal>` + `Q_OS_*` 瀹忔敼涓轰娇鐢ㄧ紪璇戝櫒鍐呯疆瀹忥紙`_WIN32`/`__linux__`/`__APPLE__`锛夛紝淇濈暀瀵?`Q_OS_*` 鐨勫吋瀹广€傝繖涓€鏀瑰姩浣挎墍鏈夐泦鎴愭祴璇?*涓嶅啀闇€瑕?Qt** 鍗冲彲缂栬瘧杩愯銆?

### 鏂板娴嬭瘯

| 娴嬭瘯 | 绫诲瀷 | 鏁版嵁婧?| 楠岃瘉鍐呭 |
|------|------|--------|----------|
| PartitionProbe | 闆嗘垚 | CFReDS dfr-01-fat.dd, DFTT #10 | SectorReader::OpenImage銆丮BR 瑙ｆ瀽銆丏etectByVbr锛團AT16/32/NTFS 鍒嗗尯璇嗗埆锛墊
| CFReDS_FAT_DFR01 | 闆嗘垚 | NIST CFReDS dfr-01-fat.dd (1GB) | FatParser 瀵?FAT12/16/32 涓夊垎鍖哄垹闄ゆ仮澶?|
| CFReDS_NTFS_DFR01 | 闆嗘垚 | NIST CFReDS dfr-01-ntfs.dd (1GB) | NtfsParser NTFS 鍒犻櫎鎭㈠ |

### DFTT FAT 娴嬭瘯淇

- `FindByName` 鍜?`HasDirectory` 鍑芥暟澧炲姞 `0xE5鈫抇` 棣栧瓧绗︾殑妯＄硦鍖归厤
- 瀛愮洰褰曟枃浠讹紙`mult2.dat`銆乣frag3.dat`锛夐檷绾т负 WARN锛團AT 鍒犻櫎瀛愮洰褰曢亶鍘嗙殑宸茬煡闄愬埗锛?

### PartitionProbe.h 璋冩暣

- `DetectByVbr` 鍜?`DetectExt4` 浠?private 鎻愬崌涓?public锛堢函宸ュ叿鍑芥暟锛屾柟渚挎祴璇曞拰澶栭儴璋冪敤锛?

### 娴嬭瘯缁撴灉

CTest 10/10 鍏ㄩ儴閫氳繃锛?.80 绉掞級銆備笉鍐嶉渶瑕?Qt 鐜鍗冲彲瀹屾垚鍏ㄩ儴鑷祴銆?

---

*锛堟湰鏂囦欢鐢卞紑鍙戣繃绋嬭拷鍔狅紝璇峰嬁鍒犻櫎鍘嗗彶鏉＄洰銆傦級*

