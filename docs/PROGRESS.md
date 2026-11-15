# 進捗表 / 引き継ぎ (PROGRESS)

**このファイルが進捗の正 (source of truth)。** 他のドキュメント (RENDERING.md など) やコード
コメントと食い違ったら、まずこの表を見て、どちらが新しいかを `git log` で確かめてから直すこと。
作業を進めたら、コミットと同時にこの表を更新する。

最終更新: 2026-11-15 (プローブ箱の基準位置を +0x110 に修正 = 壁を横切る照明の段差の原因。環境式の出力名表・入力表、霧の距離ルックアップも同日。ゲーム完成画への一致は未達: 実時刻・天候状態、室内の暗さ、FX/VFX が未解決、ゲーム画像との照合は未実施)

## 1. 現在のタスク: ゲームのレンダーパイプラインの移植

ユーザー指示 (原文):「ギミックは後にして、ゲームのSSAO・ボリューメトリクス・大気・拡散プローブ・
ポストプロセスのパイプラインを順に移植して」

| # | 項目 | 状態 | コミット / 場所 | 備考 |
|---|------|------|-----------------|------|
| 1 | エンジンパイプラインの取り込み基盤 (GamePipeline, ExternStore, TFX 評価) | 完了 | 〜7ce2de1 | `src/render/game_lighting.cpp` |
| 2 | SSAO (hdao + apply_ssao_to_light_buffers) | 完了 | 97bf077 | §3.1 |
| 3 | 大気 + 空 (sky_lookup_generate_near/far, sky, フォグ) | 基本大気・空間ノイズ付きフロクセルCSを接続 (カスケード・temporalは未) | §3.2〜3.2.3 | 原CS使用。ノイズの結線・変種選択・格子・距離等は推定 |
| 4 | ボリューメトリクス (ライトシャフト) | 完了 (密度スライス・temporal は未) | 142beb4 | §3.3。霧密度スライス (extern 88) の生成元が未特定なので基礎密度のみ = かなり淡い |
| 5 | 拡散・反射プローブ (cubemap_apply / sky hemisphere) | 全域読込・距離フェード既定オフ、CSG は解析専用。箱の位置は +0x110 (§3.4.4)。ゲームとの完全一致は未検証 | §3.4〜3.4.4 | Outpost 313、外周 167 (空レコード含む原一覧169)、低温アーカイブ499。エンジンの順序等は未解明 |
| 6 | ポストプロセス (autoexposure, bloom, LUT/color grading, FXAA, TAA) | 完了 (グレーディング層・vignette は未) | 「Map environment」「TAA」コミット | §3.5, §3.8。AE / bloom / トーンマップ / TAA の設定値は推定 |
| 6b | マップ環境 (太陽・環境光・霧の時刻カーブ = グローバルチャンネル式) | 出力名・入力 (rain_amount 等) を元データの表から解決。実行時の時刻と天候状態の選択は未特定 | 同上 + 2026-11-15 | §3.6, §3.6.1。Outpostのゲーム画像は夜。+0x08=0.5を本来の開始時刻とは断定しない |
| 6c | 空の環境光 (sky hemisphere + cubemap_apply_sky_copy_ao) = 日陰が黒い問題の修正 | 完了 (地面の照り返し・半球の霧/色調は未) | 「Sky ambient」「Cubemap volume bounds」コミット | §3.7 (箱の境界も修正) |
| 6c2 | 頭上の遮蔽 (cubemap_apply_sky_copy_ao_with_overhead_occlusion + 分散マップ) | 完了 (頭上深度は nim の描画 = 推定) | 「Overhead occlusion」コミット | §3.7.1。プローブ箱の外 (表面の約 78%) の空の光を屋根・船体の下で弱める |
| 6c3 | 初期カメラをプレイヤー開始地点 (80802D32) に | 完了 (開始地点の同定と目の高さ = 推定) | 「Start the camera at the player start」コミット | §3.7.2。開始地点の無いマップは外れ値を除いて枠取り |
| 6d | GUI マップのエンティティ表示・クラッシュ対策 | 完了 (UI 操作の実機検証は一部未) | 「GUI crash fixes and entities」コミット | §4。MapWorker に nimshade と同じモデル経路を追加 |
| 6e | 透明描画の大気透過 LUT・当該フレーム入力・モーション | 実装済み (空オブジェクトの合成は未完) | §3.9 | 原シェーダーの LUT とリサイズの GPU 検証を追加。雲等の GUI 読込は未統合 |
| 7 | 検証 (nimshade レンダー比較, テスト), コミット, push, リリース v0.4.0 | 各段で随時 | — | リリース zip は exe と README のみ |

後回し (ユーザー指示で保留): マップのギミック (アニメーション、状態変化)。エンティティの静止表示は GUI に統合済み。

## 2. 守ること (ユーザーの恒久ルール)

- ゲームリソースだけで描く。フォールバック描画は禁止。不明値は明示的な既定値にし「推定」として
  `ExternStore::note_estimate` / `estimate*` で一覧に出す (UI とログに表示される)。
- 起動中のゲームには触れない (BattlEye)。インストール先は読み取り専用。GPU/メモリキャプチャ禁止。
- ゲームデータや `oo2core_9_win64.dll` を配布しない。`out/` は gitignore (解析ダンプはここへ)。
- goriath / Deimos / Alkahest (`../goriath`, `../_deimos_reference`, `../_alkahest_reference`) は参考程度。
- 「実装して」= 型チェック・テスト・ビルド → コミット → push → (リリース) → 動作確認まで止まらずやる。
- UI はミニマル。読み込みは速く。マップはゲームと同じ見た目を目標にする。
- 返答は日本語。コミット末尾に `Co-Authored-By:` 行 (Claude の場合)。

## 3. 解析結果 (再調査不要にするためのメモ)

道具:
- `sh tools/re/dump.sh SHADER_TAG` → `out/sh/TAG.hl` (擬似 HLSL)。dxc は Windows SDK 10.0.26100.0。
- `nimcli technique TAG` (TFX バイトコードと cbuffer 初期値), `nimcli scope TAG`, `nimcli globals`,
  `nimcli read TAG [out.bin]`, `nimcli texraw TAG [out.bin]` (テクスチャの生データ),
  `nimcli map-components BUBBLE [TYPE]`, `nimcli lights BUBBLE`, `nimcli h64 HASH64`.
- グローバルチャンネル名: `src/content/object_channels.cpp` の名前表を FNV-1 で照合
  (例: ch0x0F = fog_density)。
- TFX の対応: `pop_output N` → cb0[N]、`pop_texview 20+N` → tN、extern の vec4 オフセットは ×16、
  float は ×4、texview は ×8 バイト。
- テスト用マップ: outpost バブル `80AF9550`。
  `./build/Release/nimshade.exe 80AF9550 out/x --eye 155,-60,-12 --look 125,-28,-26 --map-radius 90`

### 3.1 SSAO (完了)
- hdao_ps (80A74652): extern 0x21 が入力。t0 = デバイス深度、t1 = ビュー深度で **R と G の両方を
  Gather する** (R32 だと G=0 で全サンプル棄却 → AO=1 になる罠)。nim は R32G32 の線形深度を作る。
- 出力は extern 93 +0x00 (cubemap_apply_* が読む) と apply_ssao_to_light_buffers
  (blend state 3 = DEST_COLOR 乗算) でライトバッファに掛ける。順序は §3.4 (cubemaps → lights → apply_ssao)。
- extern 0x26 は final_combine など全画面パス共通の入力スロット。別パスで使うときは別 ExternStore に入れる。
- 調整値 (半径 10px 等) はエンジン側の値で推定。

### 3.2 大気 + 空 (実装済み)
実装: `load_map_atmosphere` (src/content/map.cpp) → `GameLighting::set_atmosphere` / `fill_atmosphere` /
`render_sky_lookups` (src/render/game_lighting.cpp)。毎フレーム near → ext7 +0xF0、far → +0xE0 を
全画面 RGBA16F に描き、シェーディング後に `sky` を深度テスト GREATER_EQUAL (gb.dsv_ro) で描く。
太陽方向は表0 (`sun_direction(p, atmosphere)`) で GlobalLighting +0x30 と nim の影にも使う。
GUI: 調整ポップアップの「大気」「マップの時刻」「時刻」。nimshade: `--time-of-day D`, `--no-atmosphere`。
未: t2 パラボロイド (白)、太陽強度の時刻変化 (ビューア値)。
以下は解析メモ。
マップのコンポーネント (outpost のノードテーブル 80AF8EBF):
- 型 **808084A0** (D2 の SAtmosphereDataComponent と同じ配置、ペイロード = レコード+0x10):
  +0x80..+0xBF WideHash×4 (lookup near0, far0, near1, far1。outpost は全部 3D テクスチャ 80AC9019:
  128×128×16 RGBA16F。**16 スライス = 時刻キーフレームの空パノラマ**, u=方位, v=0.5−dir.z/2)、
  +0xC0 2D テクスチャ 80AF8EA8 (16×1024 SRGB 真っ白 = 時刻×距離の密度 LUT と推定)、
  +0xC4 808084A5 (中身 FF)、+0xC8 float×16 = 各スライスの時刻 (0.0975 … 0.89)。
- 型 **80808346**: +0x00 = 80808349 (太陽表: vec4(0.833,0.167,0.889,0.111) + 8080ACD3 ×4。
  各 8080ACD3 は +0x30 に件数 1800、+0x70 から vec4 方向。表0 = 動く太陽 (t≈0.47 で最高)、
  表1/3 ほぼ固定方向、表2 = 月?)、+0x04 float 510000?, +0x08 = 0.5 (既定の時刻と推定)、
  +0x0C = 80808351 (技法2つ: 天体描画?)。
パイプライン (render globals):
- `sky_lookup_generate_near` (80A743BD) / `_far` (80A743C9): 全画面、VS 80A74397 がワールド方向を
  in1 に出す。PS: 3D lookup を (方位, 仰角, cb0[26]=ext7+0x70 スライス座標) でサンプル、
  2 セットを ch0x2D で補間、×cb0[23] (+0x1C8)、上書き色 cb0[35] (+0x1E0, w=重み)。
  alpha = HG 位相 (g = near:+0x160 / far:+0x1F0) × 強度 (near:+0x164 / far:+0x1F4) × lookup.w × t2.x。
  t2 = ext7 +0xC0 (2D, 方向をパラボロイド写像、回転基底 = ext 0x26 +0xC0/+0xD0/+0xE0)。
  far はレンダー前に +0x40/+0x58 を far 用 lookup に差し替える (Alkahest と同じ)。
- シェーディング (global_lighting_and_shading PS 80A74242) での使い方:
  t4 = ext7 +0x80 (密度 LUT, 座標 = (時刻 ext1+0x10, (距離−start)/(end−start)))、
  t5 = ext7 +0xE0 (遠方色: 距離 > ch0x27 (49500m) で使う → **far の出力**、空背景も同じ)、
  t6 = ext7 +0xF0 (近距離色 → **near の出力**)、t14 = ext7 +0x108 (フロクセル: x,y=透過, z=散乱)。
  T = t14.x·t14.y·exp(−距離km·密度(高さ)·LUT)、高さ密度 = 2 層の指数フォグ:
  cb0[40] = 1.4427·(ext7 +0x188, +0x1A8) = 減衰/km、cb0[41] = 1.4427·(+0x174, +0x1A4) = 密度/km、
  cb0[13] = +0x18C, cb0[18] = +0x1AC = 開始高さ、cb0[42] = LUT 範囲 (+0x178 start, +0x17C end)、
  cb0[8] = +0x170 散乱強度、cb0[109] = +0x130 フロクセル深度パラメータ。
- ext7 のフォグ値はグローバルチャンネルの写しと推定 (並び順一致):
  fog_density ch0x0F (5/km), fog_density_lookup_start/end ch0x10/0x11, fog_start_height ch0x14,
  fog_height_falloff ch0x15 (1.85), layered_fog_density ch0x18, layered_fog_falloff ch0x19 (100),
  layered_fog_start_height ch0x1A (10), fog_decay_color ch0x16, sky_color_override ch0x2C,
  sky_snapshot_rotation ch0x2E (90° → +0x1C4 = 0.25 回転と推定), sky_snapshot_intensity ch0x2F,
  sun_glow_shape/intensity ch0x0B/0x0C (far の g = 1−1/shape と推定), sky_sun_glow_* ch0x0D/0x0E。
- `sky` パイプライン (80A7439F, PS 80A7439A): 背景 = (t20.rgb + (cb0[25]·froxel.z + t20.w)·cb0[4]) ·
  cb0[9]·露出。t20 = ext7 +0xE0 (far 出力)、cb0[4] = +0x150 (太陽グロー色)、cb0[9] = +0x170。
  SV_Target4 = モーションベクトル (§3.9 で TAA に接続)。深度 = cb12[13].x → 深度テスト GREATER_EQUAL で空だけに描く。
- フロクセル (t14) は `cascade_raymarch_atmosphere_volume*` (CS, extern 0x65) が作る。§3.2.1で接続。
- 霧の検証 (2026-10-17, outpost 80AF9550): 霧のスケールは正しい。距離 m × 0.001 = km、密度 /km、
  密度 LUT (80AF8EA8, 16x1024 RGBA8) は全面 1.0、cb0[42] = (0, 1/(end−start), ext1 +0x10, −start/(end−start))
  で LUT の v は m 単位の距離。40 m で T ≈ 0.79。霧密度を 0 にしても壁は暗いまま。
  **暗さの本当の原因はマップのライティング値**: 大気ありの経路では太陽・半球アンビエントが
  レンダーグローバルの既定値 (sun_intensity 10、up/down ambient 0.1/0.15、global_ambient_intensity
  ch 0x82 = 0) のまま。日陰は拡散プローブだけになり ≈ 0.01 (日向 ≈ 0.8 の 1/80)。
  → マップ固有の値の出どころは**マップ環境エンティティのチャンネル式** (§3.6) と判明・実装済み。
  霧の距離 LUT の範囲 (fog_density_lookup_start/end = 5/10) は km と読み ×1000 (推定。LUT が全面 1 なので見た目は不変)。
  ext1 +0x10 = 時刻 (日の割合) を入れるようにした (霧の LUT の時刻列)。
実装計画: `MapAtmosphere load_map_atmosphere(pm, layout)` (content/map) → GameLighting::set_atmosphere
→ 毎フレーム 時刻→スライス座標・太陽方向 (表0) → near/far を全画面 RT に描画 → ext7 +0xE0/+0xF0 →
シェーディング → `sky` を env_bg の代わりに描画。GamePipeline に深度テストのオプションを足す。
GUI: 「時刻」スライダー + 大気トグル。nimshade: `--time T`, `--no-atmosphere`。

### 3.2.1 フロクセル大気 (2026-10-18、Claude の未完差分を継承)
- Claude 側 HEAD `2ee5a6a` は頭上遮蔽・開始地点・入力調査までコミット済み。その後の未完変更は
  CMakeの新ファイル参照、GameLightingのメソッド/リソース宣言、LightingParams.froxel_fog の3ファイル。
  `game_froxels.cpp` が存在せず、ビルド設定だけではコンパイルできない状態だったため実装を継承した。
- 原パイプライン `cascade_raymarch_atmosphere_volume` (80A742DE / CS80A742DA) と
  `cascade_raymarch_atmosphere_volume_inscatter` (80A742F4 / CS80A742F0) を使用。
  前者は密度/太陽可視性/屋内判定をRGBA16F 3D UAVへ、後者は同じxyの深さ列を前から積分して
  (透過率、屋内フォグ透過率、散乱、0) をRGBA16F 3Dへ出す。シェーダーの変更はない。
- extern0x65: +00=(W,H,D,先頭slice)、+10=(1/fx,1/fy,射影A,B)、+20=(near,far,curve,履歴重み0)、
  +30=0 (ジッター/追加の太陽減衰なし)、+40=0 (追加の距離変調なし)、+50=現在のVP (履歴未使用)、
  +90=clear-air体積 (履歴分岐は通らない)、+98/+A0=密度SRV/UAV、+A8=積分出力UAV。
- 推定: 1カスケード、画面8pxごとの格子を8の倍数へ丸める、64slice、0.5..256m、curve0.5、時間履歴なし。
  元CSは両方numthreads(8,8,1)。密度はDispatch(W/8,H/8,64)、積分は(W/8,H/8,1)。
  物理サイズと正規化を一緒に丸め、境界の余剰スレッドが有効セルを飛ばさない。
- cb12 は元 compute view scope。ただし元CSのカメラZが後ろ向きなので行6=-fwd。
  extern6+0/+C0 はnimの太陽影とカメラ空間→影clip。逆Zを使うため原CSの比較傾きは負 (推定)。
  extern81の屋内マップは未特定。雨の頭上深度の流用は下記比較調査で撤去した。影を生成しない場合は原 no_sun 変種 (80A742F9 / 80A74303) を使い、太陽散乱を含めない。屋内データなしは非屋内。
  ch0x64はsun_light_direction (FNV1=5C579DFA) で、マップの時刻に対応する太陽方向を渡す。
- 非ゼロの原データの密度/高度減衰/散乱チャンネルはそのまま。extern7+130は深度を格子Zへ戻す
  (near,far,curve,1/(far-near))。yは解析フォグの開始距離 (以前の0は積分範囲と重複)。実エンジンの距離/カスケードは未確定。出力は同一フレームのextern7+108とextern40+78へ接続。
  GUI「フロクセル大気」、nimshade `--no-froxels` で無効化可能。
- GPU read_texture は3Dの全深度スライスを行ピッチに沿って読み戻すよう修正 (従来は先頭sliceだけ)。
  `--verify-frame-inputs` に全sliceのfinite/非負、透過率0..1、深さ方向の単調減少、
  不透明/透明パスのリソース一致を追加。Outpost320×180→337×191→320×180で成功、D3D debug0B。
- 影なしで通常の太陽影つきCSへ白テクスチャを渡す経路は device removed を再現したため使用しない。
  元の no_sun 変種への切替後、Outpostの影なし描画・全slice検証が成功。`--no-taa` で
  前フレーム行列を更新しない場合はリサイズ検証で回転モーションを要求しないよう修正。
- 最終検証: Release nim/nimshade/nim_render_tests、ctest 2/2。Outpost影あり/なし、低温アーカイブ全域
  (249,991静的配置、4,656エンティティ配置、7369ライト、499プローブ) の影あり/なしで
  TAA16フレームと320×180→337×191→320×180を実行。低温全域はテクスチャ上限128で検証。
  全64sliceのfinite/非負/透過率0..1/距離方向の単調性、不透明・透明の同一volume接続を確認、D3D debug0B。
  GUI更新版は `out/continued-bin/nim.exe`。本ターンで正式リリースはしない。
- 未検証: ゲームの実カスケード構成、時系列再投影、距離変調・影の調整値。ゲーム最終画像との一致の証明ではない。

### 3.2.2 ゲーム完成画像との比較 (同日)
- ユーザーの2枚: 現状は青いかすみが全体を覆う。ゲームは暗い屋外の中で白・黄・橙の局所光が分離している。
  ユーザー確認: 現状画像は時刻を夜へ変更して撮影。**Outpostは本来夜のマップ**。
  比較の途中でこちらが初期0.5の昼を描いた画像は同条件ではない。夜の検証用に0も描いたが、ユーザー撮影時の正確な値とゲームの実行時値は不明。
- 80808346 +0x08=0.5を「マップの時刻」としていたが、フィールドの意味もアクティビティ側の開始時刻も未検証。
  GUIを「初期時刻を使う (推定)」に変更。Outpostだけ任意の時刻へハードコードはしない。
- `out/comparison/` に全域Outpostの大気あり/フロクセルなし/大気なし/夜を出力。
  eye=(287,-40,21)、look=(145,15,-25) は同じ中庭への近似画角、800x450、テクスチャ上限128、exp_outpost。
  露出固定1の切分けでは大気を除くと局所光・黄色塗装の明暗が出る。**プローブの件数だけで最終照明を再現できたとは言わない**。
- 修正: inscatter CS のcb0[74].zwはextern7 +0x180/+0x184の差で正規化する。
  従来両方0でTFXが1/0・0/0を生成。追加距離変調は無効のまま、有限の既存LUT範囲を明示的な推定として渡す。
  密度/積分CSのTFX定数がNaN/Infなら、シェーダーへ投入したコマンドを送信する前にエラーにする。
- 修正: 解析フォグは元PSで max(distance - extern7+130.y,0) に適用される。
  y=0から単一体積の終端へ接続し、積分済みの手前の範囲へ再び解析フォグを掛けない (体積の距離設定自体は引き続き推定)。
- 修正: extern81 (interior) に雨深度を入れていたが、原パイプラインは main/interior/occlusion が別入力。
  元のinteriorデータを見つけるまで非屋内と明示する。**この修正後も濃いかすみが残り、画像の差の主因を解決したとは言えない**。
- `--dump-lighting` にnear/far空とフロクセルのRGBA16F全slice、および各サイズ/形式のJSONを追加。
  `--verify-frame-inputs` は屋内入力なしの体積でy透過率=1も検証する。
- 検証: Release nim/nimshade/nim_tests/nim_render_testsをビルド、ctest 2/2成功。
  夜=0のOutpost全域640x360 (上限128、exp_outpost) のTAA16フレーム、657x371→640x360、
  全64slice・LUT・モーション検証成功。dumpのサイズ/JSONをPythonで照合、全成分finite、屋内透過率は全セル1。
  別途320x180の影あり/なしをD3D12 debug付きで実行、両方の全slice/リサイズ検証成功、両debugログ0B。
  新GUIは `out/night-inputs-bin/nim.exe` (コンパイル確認、今回のUI操作は未検証)。ゲーム完成画への一致は未達のため正式リリースなし。
- 次: 夜のアクティビティ実行時環境入力、霧の距離変調と密度変種、未移植のグレーディング層を元データから追う。
  新しいCSが動作することとゲーム完成画への一致を区別する。夜でもかすみ・光の分離は未解決。

### 3.2.3 空間ノイズとFrame入力の再調査、1層プローブの修正 (同日)
- 700原技法・734シェーダーを `nimcli render-resources` で再索引。
  参照ゲームのFrame配置をそのまま使うことはできない。
  元uber-lightはFrame +0xC0をTexture3Dで読み、global shadingの+0xD0は薄膜/玉虫色のRGB表を読む。
  ノイズ付き大気CSは **Frame +0xD8をTexture3D**で読むが、nimはここに2Dのiridescence表を入れていた。
  C0=3D lobe、D0=iridescenceへ修正。B8=lobe/C8=tintの未使用スロットは配置からの推定として記録する。
- 共通データ `render_globals.data +0x40 = 80A74EAC` は **48x48x48 BC4_UNORMのスカラーノイズ**。
  これをFrame +0xD8へ接続。元のエンジン結線コードは未取得のため、**この結線の同定は推定**としてUI/logへ出す。
  原BC4のままGPUへロードし、元CSが元サンプラーで読む。任意の生成ノイズ・色調変更・密度倍率は追加していない。
- 原密度CSのnoise変種 `80A742E9 / CS80A742E5`、no_sun変種 `80A742FE / CS80A742FB` を実装。
  元の均一変種はfog_density.xy / layered_fog_density.xyの平均値を使う。
  noise変種は2スケールの3Dノイズ、元chBA/BBの移動・サイズ、chBC..BEの強さ/高さ、
  密度チャンネルzwのリマップを使用。Outpostはfog_density=(3,20,0.45,0.6)なので平均密度とは異なる。
  選択条件はエンジン側未特定の推定。既定でnoiseを使い、`nimshade --no-froxel-noise`で元の均一変種と比較できる。
- 同一Outpostの夜=0、eye=(287,-40,21)、look=(145,15,-25)、640x360、固定露出1、テクスチャ上限128で比較。
  表面位置でのフロクセル透過率 (解析フォグ・散乱色・ポストを含まない) の中央値 **0.2138→0.3012**。
  透過率0.5未満の表面は100%→96.9%。均一なかすみは弱まるが、**依然大半の表面で半分以上減衰している**。
  ゲーム完成画像との一致・実ゲームでnoise変種が選択されることの証明ではない。
- `tools/re/analyze_atmosphere.py` を追加。`--dump-lighting`のRGBA16F体積を元PSの正規化Zで
  trilinear/clampサンプルし、表面の透過率分布とPNGを出す。JSONに実near planeとfroxel_mappingを追加。
  古いdumpへ0.05を仮定しない (比較時の旧headless nearは30mだったことをVP行列から確認)。
  headlessのマップnearをGUIと同じ0.05mへ統一。全域の半径が近クリップ面を30mに動かす不具合を除去。
- **プローブの描画脱落**: `Gpu::texture`がdepth>1だけをTexture3Dと判断していた。
  diffuseプローブの原PSはZが1層でもTexture3Dを要求するため、元グリッドを保持して3Dで作る経路を追加。
  Outpost: index90/91 (11x8x1、8x12x1)、Perimeter:111/112 (1x22x1、1x9x1)、
  Cryo:250/251/370 (2x3x1、3x2x1、12x4x1)、**計7個の有効な原プローブ**が該当。
  Outpostの上記画角で236→238個を描画、`missing t2`が消えた。全域の覆いが完成したという意味ではない。
  `--verify-frame-inputs`はプローブ描画失敗も検出する。
- 検証: Release nim/nimshade/nimcli/nim_tests/nim_render_tests、ctest 2/2。
  Outpost全域のnoise/均一変種、Perimeter全域(影あり)、Cryo全域(影なし)で
  全sliceのfinite/透過率単調性/屋内入力なしのy=1、TAA、大小リサイズを確認。D3D debugは全て0B。
  最終Outpostはnear0.05、800x450→817x461→800x450でも成功。
  三線形の端/クランプ/中心を既知の2x2x2データで確認。全域テクスチャ上限は128 (GPU原プローブ/ノイズはフルサイズ)。
  GUIは `out/spatial-fog-bin/nim.exe`へビルド済み。今回のGUI実操作は未検証、正式リリースはしない。
- 未解決: 実行時の夜/天候入力、noise変種の選択条件と元結線の確証、霧の距離変調・カスケード、
  グレーディング層。ゲーム完成画の色/明暗への完全一致はまだ達成していない。
- 解析上の注意: `inspect_tfx_bindings`は最後のtexview元を追う簡略解析。
  80A7433Eのt4は実際には`push_extern_u32 05 20` (実行時ハンドル)なのにC0として表示される。
  同一C0が2D/3Dを兼ねる根拠には使わない。実行時ハンドルの解決は別途必要。

### 3.3 ボリューメトリクス (実装済み)
実装: `GameLighting::render_volumetrics` (src/render/game_lighting.cpp)。順序 (シェーディングと空の後):
1. `downsample_full_depth_to_quarter_max_min` (extern 35: +0x00 深度, +0x10 出力サイズ, +0x20 入力サイズ,
   +0x30 = (0, 1/near)) → 1/4 解像度 RGBA32F (x = 最大ビュー深度, y = 最小, z = 平均, w = 平均の位置)。
2. ライトごとの volumetrics 技法 (MapLight::volumetrics) をライトボリュームとして 1/4 解像度で加算描画。
   ビューは 1/4 解像度に切り替え (fill_view_externs + View +0x2E0 + cb12 再評価)。Deferred +0x98 = 1.
   の min/max (transparent スコープ t10)。extern 50: +0x40 = (画素, デバイス深度) → ライト箱空間,
   +0x80 = 箱空間の視点, +0xB0 = 1, +0xB4 = 1 (推定)。extern 8 +0x00 = 箱 → ワールド。cb1/cb2/cb8 は
   rigid_model / transparent / transparent_advanced スコープを都度評価。出力 o0 = 遠 (max 深度まで),
   o1 = 近 (min 深度まで), o2/o3 = 透明物用の 8 スライス (今は未使用)。
3. `volumetrics_blur_results` (extern 38: +0x00 遠, +0x08 近, +0x18 min/max, +0x50 1/4 サイズ;
   extern 89 +0xA0 = 1 でブラー有効) → (遠, 近)。
4. `volumetrics_upres_1` (extern 38 同上 + +0x50 フル, +0x60 1/4, +0xC0 = 1; Deferred +0x98 = nim の
   R32G32 線形深度) → HDR に加算 (技法にブレンド状態が無いので加算は推定)。終わったら extern 38 +0x00 と
   Deferred +0x98 を元に戻す (final_combine と他パスが読む)。
未: 霧密度スライス extern 88 t0/t1 (0 = 推定。基礎密度 1e-4/m だけなのでとても淡い)、temporal filter
(extern 89 の履歴。今はディザ 0 で縞が少し出る)、assemble (透明物用)、t20/t21/t25 の大気ボリューム。
以下は解析メモ。
- ライトごとの技法: SShadowingLight は +0xD4 shading, +0xD8 shading_shadowing, **+0xDC volumetrics**,
  +0xE0 volumetrics_shadowing, +0xE4/+0xE8 lightprobe CS。SLightCollection の各ライト (0x100 バイト)
  は +0xC4 shading, **+0xC8 volumetrics**, +0xCC lightprobe CS。
- ボリューメトリクス PS (例 80AF3014): 光の円錐と視線の交差区間を 64 ステップ積分。
  入力: extern 0x58 (volumetrics_pass) t0/t1 = 視線方向の霧密度 8 スライス (sqrt(距離/64m)·8)、
  密度 = cb0[31]·スライス + cb0[30] (技法定数 1e-3, 1e-4。スライスが 0 だとほぼ見えない)、
  extern 0x32 (volume_fog: +0x40 画素+線形深度→ワールド行列, +0x80 ライト空間の視点, +0xB0, +0xB4 符号)、
  extern 4 (deferred_light, 照明と同じ), transparent / transparent_advanced スコープ (cb2, cb8;
  t10 = 縮小深度 min/max + .w 影, t20/t21/t25 = 大気系ボリューム)。
  出力: o0/o1 散乱色、o2/o3 = 8 スライスへのエネルギー配分 (加算)。
- 後段: volumetrics_assemble_intensity_slices (8 スライス → 累積分布, 透明物用),
  volumetrics_blur_results, volumetrics_temporal_filter_results (extern 0x59 履歴),
  volumetrics_upres_1 (双方向アップサンプル: t0/t1 = 低解像度 min/max 深度の結果, t2 = 低解像度
  (min,max) 線形深度, t3 = フル解像度深度 (R,G 平均)) → HDR に合成 (ブレンドは未確認)。
- 霧密度スライス (extern 0x58) の生成元は未特定 (マップのフォグボリューム系エンティティと推測)。

### 3.4 拡散プローブ + マップのライティング (適用を実装、ゲームとの完全一致は未検証)
- 不透明物の拡散プローブ = キューブマップボリューム (cubemap_apply_*_probes_on, PS 例 80A74B26)。
  プローブは R8G8B8A8_UNORM の 3D テクスチャ (x 方向に 7 枚: 平均 + ±X/±Y/±Z)。PS の出力
  o0 (拡散 → Deferred +0xC0) = 露出 cb13[1].x × global_cubemap_intensity (ch 0x83) × probe × (1 + k·probe)
  × AO^(2·ao_ambient_weight ch 0x54) × 強度 (+0x30) × フェード, o0.w = 1 - 重み。o1 (鏡面 IBL → +0xD0) も同様。
  alpha = 1 - 重み + blend 23 (src + dst·src.a) は「下にあるものを置き換える」合成なので、ボリュームは
  ライトより先に描く (後だとローカルライトを消す)。順序: cubemaps → lights → apply_ssao (推定。プローブは
  AO を自前でも読むので二重に掛かる)。cb0[14] = (ch 0x80, ch 0x81 - ch 0x80), cb0[16] = ch 0x8B。
- light_sample_local_cubemaps_diffuse_probes / light_sample_sky_hemisphere は CS (extern 0x44 の UAV) で
  透明物・パーティクルのライト用。不透明物には効かないので透明物の移植時に一緒にやる。
- GlobalLighting (extern 42) はマップの大気があるときマップのチャンネルから作る: +0x10 = sun_color (0x09) ×
  sun_intensity (0x08), +0x30 = 太陽の経路方向, +0x70 = up_ambient_color × intensity (0x57×0x58),
  +0x80 = down (0x5A×0x5B), +0x90 = (up_sharpness 0x59, down_sharpness 0x5C)。シェーディングは半球
  アンビエントに global_ambient_intensity (0x82, 既定 0) を掛けるので、マップでは拡散アンビエントは
  ほぼプローブだけ。+0x50 / +0xD0 は意味だけ読めていて値は推定。大気が無い (ビューア) ときは従来の設定。

### 3.4.1 全域の原プローブ調査 (2026-10-18)

最新のユーザー指示:「見た目を近づけるんじゃなくて、あくまでも絶対にゲーム内に忠実に、全範囲のプローブを見つけ出して」。
暗い CTRL 壁を基準に全体を暗くする、室内プローブを領域外へ延長する等の調整はしない。

- `nimcli probe-audit 80AF9550 out/outpost-probes.tsv` を追加。バブルの経路とは独立に
  `w64_outpost_` 全パッケージの **2,441 ノードテーブル**と cubemap collection (80807F4B) を列挙する。
  読込失敗は中断、一覧の差分・テクスチャ読込失敗・寸法不一致・実行時ローダー件数不一致は終了コード 1。
- 原一覧 **80AE8026**: +0x10 Vec<80807F50, stride 0x1D0> に **313 領域**。
  バブルから到達する 88 テーブル中の 313 個の 80807F3C と、ヘッダー以外の **+0x10..+0x1CF をバイト単位で全件一致**。
  全 2,441 テーブルにも追加の 80807F3C はない。実行時ローダーも 313 領域。
- 各 +0x160 の grid xyz 積の総和は **703,505** (収録グリッドのスロット数。全点が有効な照明点という意味ではない)。
  313 個の diffuse texture を全件ロードし、寸法が **(7×grid.x, grid.y, grid.z)** と一致。
  各 specular cube / 指定された alpha texture もロード検証。読込失敗・寸法不一致とも 0。
- TSV は領域ごとに元 collection/index/ID (+0x1C8)、対応ノードとオフセット、中心、grid、
  全 texture 参照・寸法、local_to_world 16 要素、付属タグと件数、vertex buffer 参照を保存する。
  出力はゲーム由来データなので `out/` のみ。追跡・配布しない。
- 原一覧 +0x30 は **80A7995A** (type 11 / class 8080A337)。Havok `SDKV20180100` バイナリであり texture ではない。
  空間データの正確な役割・検索条件は未解析。
- 各領域 +0x1B4 の **80807F54** 付属タグを全件取得: 163 種、+0x08 の Vec に合計 **624 レコード**。
  非空のタグは 162 種、空の共通タグ 80A7995B は 151 領域で共有。
  レコードは 0xD0 バイト、class 80807F57。役割は未確定で、追加の拡散プローブとして数えない。
  162 領域は +0x1C4 の vertex buffer も参照する (例 80A7996E: stride12 / 3456B の float3 データ)。
  この調査時には未使用。後続の §3.4.2 で境界描画を試したが、通常の照明への適用判断は撤回した。
- ローダーの重複排除を「行列 + float に変換した specular hash」から原 payload の全バイト比較へ変更。
  float 変換による hash 下位ビットの消失と、同じ配置・反射 cube で異なる diffuse/flags を持つ領域の誤統合を防ぐ。
  Outpost は修正前後とも 313 領域で、これ自体による見た目の変化はない。
- 検証: Release nimcli/nimshade/tests と別出力先 GUI をビルド、ctest 2/2、上記実データ監査を実行。
  発見済みのクラスの収録範囲を照合した結果であり、未解明の付属データやエンジン側の適用条件を含む
  「ゲーム内と完全一致」の証明ではない。次はこれらの適用条件・形状、sky ambient との合成を解析する。

### 3.4.2 CSG 境界の幾何検証と 3 マップの全域監査 (2026-10-18、描画への適用判断は撤回)

**訂正:** 以下は `14e4e6e` 時点の実験記録。付属 CSG の幾何学的な一致を確認したが、それが照明の適用境界であること、VS 基準位置が +0x110 であることは確認していない。通常描画への制限と位置変更で CTRL 壁の照明が退行したため撤回。通常は +0x20 と元のボックス描画経路を使い、CSG は `--dump-probe-mask` の解析時だけに限定する。下記の「誤りを修正」「適用形状」の断定は根拠不足だった。

- **+0x20 は境界中心、描画の基準位置は +0x110**。+0x110 は +0xC0 行列の translation および
  +0x120 軸行列の translation と対応する。従来の VS cb11[0] が +0x20 を使っていた誤りを修正。
  Outpost は 266/313 領域で両者が異なる。PS のグリッド解像度・テクスチャ・強度は原データのまま。
- +0x1B4 の 80807F54 は +0x08 Vec<80807F57, stride 0xD0>。
  レコード +0x10 は CSG operation (観測値 0 = 加算、1 = 減算)、+0x30 は unit box → world 行列、
  +0x20 は half extents、+0x70 は軸、+0xB0 quaternion、+0xC0 origin。
  +0x1C4 の float3 頂点列はその合成境界 (triangle list)。Outpost の例 80A7996E で
  全96面の重心が原 CSG 境界面に一致 (local 誤差最大 3.81e-6)。追加プローブではなく適用形状。
- 元の volume VS (80A74B13) は input を反転するので、原頂点の符号を反転して渡し、
  原 CSG 座標をそのまま再現。形状バッファはタグ単位で保持し、マップを切り替えても描画中の GPU 参照を破棄しない。
- **ステンシルの深度失敗時の winding count** でシーン表面が原境界内にあるかを求め、
  count != 0 の画素だけ既存の原 cubemap_apply PS で描く。凹形状の裏面をそのまま重ねて多重合成しない。
  マスクは原 VS のみ (PS なし)、元の色や照明値を置換するシェーダーは追加しない。
  エンジン側の stencil state/スケジューリング自体は未解析なので `note_estimate` に明記。
- GBuffer は浮動小数32bit深度を維持し stencil plane を持つ D32_FLOAT_S8X24_UINT へ。
  depth SRV は R32_FLOAT_X8X24_TYPELESS。マスク時は stencil plane のみ WRITE に移し、depth plane は
  READ + PS_RESOURCE のまま。影マップは従来の D32_FLOAT。空・透明物・SSAO・TAA の深度参照も更新。
- GUI / nimshade は選択マップの **全プローブ**を準備して描画時に視野・距離で絞る。
  nimshade の static/terrain/light の読み込み中心は指定 --look、次に --eye、未指定時のみ map_focus。
  従来はカメラを移しても map_focus 周辺の形状しか読み込まれず、他区域の検証が空振りしていた。

| マップ / bubble | 原一覧 | テーブル数 | 原領域数 / 描画可能数 | grid slots | 境界 mesh | 付属 CSG records |
|---|---|---:|---:|---:|---:|---:|
| Outpost 80AF9550 | 80AE8026 | 2,441 | 313 / 313 | 703,505 | 162 | 624 |
| 外周 (Perimeter) 80A8FE73 | 80A653B0 | 3,017 | 169 / 167 | 367,463 | 109 | 414 |
| 低温アーカイブ (w64_marathon_deck_001_) 80B385FA | 80B1FD04 | 4,061 | 499 / 499 | 2,888,259 | 176 | 659 |

- 全マップとも原一覧と全ノードの payload 比較差分0、必要な texture の読込失敗0、grid寸法不一致0。
  外周の原一覧 index77/78 は specular/diffuse が両方 FFFFFFFF の空レコードで、描画データを持たない。
  外周 index53、低温 index50/400/403 は grid=0 / diffuseなしの **反射専用**。原 probes_off 技法で描く。
  監査はこれらを読込失敗と誤判定しないよう修正。TSV に origin と vertex count も保存。
  447 個の原境界 mesh の座標最大絶対値は 1.0000153、全 CSG operation は 0/1 のみ。
- `nimshade ... --probe-index N --dump-probe-mask --no-taa` で、単一の実行時プローブの
  stencil plane / scene depth / カメラ行列 / 原 CSG を **nim 自身の GPU 出力から** `out/` に保存できる。
  N は実行時ローダー順であり原 collection index と同一とは限らない。
  `python tools/re/verify_probe_mask.py OUT...` (NumPy) は、深度から復元したシーン位置と原 CSG を独立に照合。
  境界の float 精度帯を除外し、内外の両方を通る検証画像群を要求。不一致があれば失敗する。
  Outpost の室内・外側・切り欠き、外周、低温で計 **287,988 表面画素、不一致0**。
- 検証: Release ビルド、ctest 2/2、3マップの全プローブ準備と headless 描画、D3D12 debug 警告/エラー0、
  LUT と 657×371 → 640×360 リサイズ後の深度受け渡し・モーションも確認。
  GUI ビルドは `out/probe-bin/nim.exe`。8秒の起動 smoke check と debug log 0件を確認。
  既存の起動中ビューアは差し替えない。
- 未解明: エンジンの領域の優先順位・描画順、Havok 空間検索、relighting の完全な実行時入力、
  sky hemisphere の地面反射/霧/色調、既存のフェード等。ここまでの変更は原形状の適用を復元したもので、
  ゲーム最終画像との完全一致の検証ではない。

### 3.4.3 全域で原プローブを描画するための修正 (2026-10-18)

- ユーザー指示は「CTRL 壁だけ戻す」ではなく、全マップを各地の原プローブ照明で描画すること。
  通常描画の CSG 制限を外して CTRL 壁の復帰を headless 1280×720 で確認した。
- 313 / 167 / 499 領域を全件準備する経路は維持。原色・強度・グリッド・空間フェードは変えない。
  カメラ (-138,-75,256) の Outpost 俯瞰で、旧処理は 313 領域をロードしても距離フェードで描画 **0 件**。
  +0x3C/+0x40 をカメラ距離に使う条件は未検証なので既定オフとし、全域表示では cb11[14].w=1。
  修正後は同じ俯瞰で **311 領域**を描画 (残りは視野外)。室内プローブを他地域へ延ばしていない。
  GUI「プローブの距離フェード (推定)」と nimshade `--probe-distance-fade` で旧推定を選択可能。
  これは全域観察のための設定であり、ゲームのカメラ距離制御を復元できたという主張ではない。
- culling の AABB は補助行列に加えて通常 VS の +0x20 中心・+0x100 quaternion・+0x10 extents で
  実際に描く箱も包含する。補助 origin と中心が異なるために可視領域を誤棄却しない。
- エンティティの rigid VS `80A499C1` は cb1[8..11] を前フレームの配置として出力する。
  nim はこれを identity のまま残していた。静止配置では cb1[0..3] をコピーし、現在と前の配置を一致させる。
  `--verify-static-motion` は静止材質・TAA 16 フレームの RT3.xy を実 GPU で全画素検証 (上限 0.1 px)。
  同一条件 (Outpost, 640×360, 半径65m) の最大値は修正前595.12714px、修正後0.0008056714px。
- DEIMOS の `src/ui/tabs/map.rs`, `activity.rs`, `crates/ecs/src/world/map.rs` を参照し GUI に
  `map_view` / `activity_view` 選択を追加。前者は SBubbleParent の定義コンテナのみ、後者は SActivity の
  選択マップ行の map_references[0] と、同じ bubble_name の ambient 行および phase テーブル。
  表示に元 activity hash / map index / bubble hash を出す。実データの map-row stride は0x38
  (参照リポジトリの0x48注釈とは異なる)。宛先接頭辞のない exp_outpost 等の名前も含む。
  Marathon の phase 定義 class は古い DEIMOS と異なるため、選択した phase component 内の
  型を検証できるノードテーブル参照を列挙する。実行時の phase 状態遷移は未実装。
- 従来はエンティティだけ全パッケージファミリーから読み、無関係の activity を混ぜていた。
  選択した MapLayout.node_tables 内だけに制限。Outpost map_view は88テーブル、1142配置 / 87モデル。
  activity_view の exp_outpost は2442テーブル (追加2354)、3309配置 / 202モデル。
  `nimcli activities --verify` で19選択肢の phase/ambient 解決と layout/probe 読込を全件確認。
  nimshade `--activity HASH --activity-map N` も同じローダーを使用する。
- スパークは exp_outpost のモデル `80B58B0D` (3配置) 単体を残しても再現、同モデルを外すと消える。
  `PrepareCache.gbuffer_only` が static には効くが dynamic の stage8 には効いていなかった。
  動的モデルも同じ stage 選択を使い、GUI の不透明経路へ透明 FX が紛れないよう修正。
  本体の G-buffer パートは残す。完全な透明 FX の状態入力を復元した修正ではない。
  全202モデル入りの同じ俯瞰で放射状スパークの消失を画像確認 (640×360)。
- 最終検証: Release nim/nimshade/nimcli/tests ビルド、ctest 2/2、3マップの全件 probe-audit が成功
  (原一覧との payload 差分・texture 読込失敗・寸法不一致0)。19 activity 選択も全件成功。
  headless: Outpost俯瞰311/313、CTRL壁111/313、外周俯瞰10/167、低温俯瞰99/499領域を描画。
  全マップで全件準備し、視野内だけを描画。各 D3D12 debug log は0B。
  CTRL壁の静止TAA16フレームは最大0.001px未満。GUI `out/map-activity-bin/nim.exe` は
  別出力先へビルドし、専用の8秒起動 smoke check で生存・debug log 0Bを確認。UI操作は未検証。
- 解析出力は `out/` 内のみ。CSG が照明に必要か、合成優先順位、sky hemisphere の詳細は依然未確認。
  全テクスチャ・原領域の描画準備を確認したもので、全表面が有効サンプルを持つことやゲーム最終画像との
  完全一致を証明したものではない。既知の空合成などの未実装が残るためv0.4.0正式リリースは見送る。

### 3.4.4 プローブ箱の基準位置は +0x110 (2026-11-15、照明の段差の修正)
ユーザー指摘 (原文):「ライトプローブみたいなのが存在してるところとしてないところがあって、ガクッとライティングが変わってしまっている」。
- 原因: 箱を +0x20 に置いていた。箱の実体は行列 +0xC0 (単位立方体 → ワールド) で、その平行移動が +0x110。
  クォータニオン +0x100 × 半径 +0x10 は行列の 3 列と全領域で一致する (最大差 Outpost / 外周 7.6e-6、低温 9.2e-5。
  `nimcli cubemaps BUBBLE` の末尾に出る)。+0x20 は別の点で、Outpost 266 / 313、外周 140 / 167、低温 417 / 499 領域で
  +0x110 と違う (数 m。例: Outpost #1 は 6.5 m、#2 は 10 m)。PS は VS の座標系でグリッドを引くので、
  プローブのグリッドごと数 m ずれ、室内用の暗い値が外壁に、外の値が室内に掛かっていた。
- 判定は見た目ではなくデータ: 重なり合う領域同士を同じワールド点でサンプルした平均プローブ輝度の相関が
  +0x20 基準で 0.335、+0x110 基準で **0.817** (Outpost、約 380 組・5 万点。平均絶対差は 0.065 → 0.034)。
  Alkahest (D2 エンジンの逆コンパイル由来) も VS にはノードの平行移動を渡し、+0x20 は使っていない。
  §3.4.2 の「+0x110 が描画の基準位置」は正しかった。撤回したのは CSG 制限と抱き合わせで、CTRL 壁の見た目を理由にしたため。
- 結果: CTRL 建物の外壁を横切っていた矩形の段差 (左が暗い赤、右が空の色) が消え、壁全体が空の環境光、
  室内 (窓・扉の奥) が室内プローブになる。**従来「ゲーム本来の色合い」とされた CTRL 壁の暗さは、このずれが作っていた**
  (§3.7 の「残り」、§3.7.1 冒頭の前提と食い違う。ゲーム画像での確認が必要)。
- 縁の硬さ: Outpost の 195 / 313 領域は 6 面ともブレンド幅 0。Alkahest の定数も幅 0 を係数 1000 (= nim の 0.1%) にする。
  縁を柔らかくする調整はしない。302 / 313 は cb11[15].z = 0 でプローブの α (方向別の遮蔽) を重みに使わない。
- 同じ逆コンパイル由来の式を採用 (推定として表示。Marathon の PS が同じスロットを読むことは確認済み、エンジン側コードは未確認):
  - cb11[15].z = 2z (z ≤ 0.5)、60·(2z − 1)⁴ + 1 (z > 0.5)。従来は生の値。Outpost で該当 11 領域。
  - relighting 変種の cb11[19] = cubemap_bounce_scale (ch 0x8B) × +0x170.x × r、cb11[20] = cubemap_relighting_sky_intensity (ch 0x8A) × +0x180.y + r、
    r = (max(sun_light_direction.z, 0.5) × ch 0x89 (8F552B79) × sun_intensity / 10 × sun_color − 1) × +0x180.x + 1。
    従来の基準値 (cb20 = 1) を置き換え。Outpost は全領域 +0x180.x = 0 なので太陽の項は効かず、空の項だけ (7 領域)。
    sun_light_direction (ch 0x64) は nim が太陽を書き込んでいない (既定 (0,0,1))。
- 未対応: +0x70.w > 0 の領域 (Outpost に 1 個) はエンジンでは cube_sphere 変種。nim は cube で描く。
  描画順 (大きい順) と距離フェードは推定のまま。箱の外 (表面の約 78%) は空の環境光で、その強さ
  (sky_color_override、cubemap_sky_intensity、頭上の遮蔽) がゲームと合っているかは未検証。
- 検証: ctest 2/2、`--verify-frame-inputs` Outpost / 低温 終了 0、外周描画 終了 0。

### 3.5 ポストプロセス + アンチエイリアス (実装済み, `src/render/game_post.cpp`)
- 自動露出: autoexposure_sample_columns (1/16 の bloom 段の列ごとの log 輝度和) を読み戻し、key 0.18、
  2^ch05..2^ch06 にクランプ (推定)。bloom: bloom_initial_downsample → downsample ×4 → gaussian_10 縦横 →
  weighted_add → copy_texture_bilinear、強度 0.04 (推定)。トーンマップ: screen_area_global_lut3d mode 4 +
  恒等 32³ LUT (グレーディング層のデータは未発見)。
- FXAA: エンジンの `fxaa` (80A7434E, PS 80A7434A = FXAA 3.11 quality)。入力 t0 は「RGB = 色, A = 輝度」の 1 枚。
  トーンマップは輝度を SV_Target1 に出すので、同じパイプラインをもう一度、target0 をマスク 0、target1 =
  入力テクスチャの A だけ書くマスクで描いて合成する。extern 0x33: +0x00 入力, +0x50/+0x54/+0x58 =
  subpix 0.75 / edge 0.166 / edge min 0.0833 (FXAA 既定値 = 推定)。他に smaa_*, cmaa_*, resolve_taa* もある
  (TAA は §3.8)。nimshade `--no-aa`、GUI「アンチエイリアス」。

### 3.6 マップ環境 = グローバルチャンネル式 (実装済み)
- 経路 (outpost 80AF9550): マップ → 80AE802C (8080A7BB) → hash64 → 80AF8EE3 (8080A7C1) → 80AF8EA7
  (8080B1A7 = SMapNodeTable) のノード +0x30 hash64 → 環境エンティティ 80AC8FFC (8080BAAD) → コンポーネント
  80AC8635 / 80AC8638 (8080BADB)。つまり普通の「ノードに置かれたエンティティ」で、`load_map_atmosphere` が
  全ノードのエンティティを辿り、コンポーネント内の 8080B64E レコードを集める。
- 8080B64E (D2 SSequenceGlobalChannel 相当, payload = クラス + 4): +0x00 チャンネル名 FNV-1 ハッシュ
  (render globals 80A74013 の channel id)、+0x14/+0x18 = 3600/3600 (開始/周期 s)、+0x30 バイトコード vec、
  +0x40 定数 vec。outpost は 75 件中 55 件が render globals のチャンネル (残りは名前なし / オブジェクト用)。
- シーケンサ VM (Alkahest sequencer_vm): 0x01..0x48 はマテリアル TFX と同じ。0x49 = gradient8 (goriath
  bytecode_op_unk3b_const: 基準 k0, 停止点 0..3 の差分 k1..k4, 4..7 の差分 k5..k8, 閾値 k9/k10, 最後は 1 まで)、
  0x4A n (2 byte) = 入力 n、0x4C n (2 byte) = 出力 n (チャンネル slot + n)。`TfxEnvironment::sequencer_input`。
- 入力 0 = 時刻 (日の割合 0..1, Alkahest time_of_day/3600)。入力 1 は霧密度などが `saturate(0.0101·(in1 − 1))`
  で読む 1..100 の値 (天候の強さ?)、出どころ未特定 → 0 (= 平常, 推定)。
- 毎フレーム `GameLighting::update_global_channels`: 既定値 → 環境式 → 天気の上書き。マテリアルにも
  `StaticMeshScene::set_global_channel_values` で同じ値を渡す。nimshade `--no-environment` / `--print-channels`、
  GUI「マップ環境」。
- outpost 正午の値: sun_color (1,1,1), skybox_sun_color (1,.97,.94), up_ambient (.48,.62,1)×4,
  down_ambient (.89,1,.72)×0, global_ambient_intensity 0, fog_density 3/km, fog_height_falloff 0.1,
  layered_fog 0.1 (falloff 100, start −28 m), ae_min/max −1/+1。チャンネル名はハッシュ総当たりで確認済み。
- **残る見た目の問題**: 大気 (= マップのライティング経路) を切ると白壁・黄色塗装が出るが、入れると日陰の壁が
  ほぼ黒。霧ではない (霧 LUT を変えても不変)。0x82 = 0 なので日陰はキューブマップのプローブだけで、
  → 原因は §3.7: プローブは正常 (平均 0.05〜0.3、ボリュームは 2〜17 m の屋内用) で、屋外の壁はどのボリュームにも
  入らない。エンジンは空の環境光パス (cubemap_apply_sky_copy_ao) を全画面に先に描いており、それが未実装だった。

### 3.6.1 環境式の出力名表と入力表 (2026-11-15)
道具: `nimcli environment BUBBLE [ACTIVITY] [-v]` (式を持つ全エンティティを、ノードテーブル・コンポーネント別に列挙し、
1/8 日刻み × 入力 1 = 0/1/100 の値を出す)。
- **8080B64E レコード先頭の語はチャンネル名ではない。** 出力の行き先は、同じコンポーネントの出力表
  (`{self tag, 80809F46, ..., +0x30 = FNV-1 名}` が出力番号順に並ぶ。コンポーネント +0x1A8 の Vec<80809F47> が指す) を
  レコード +0x28 の出力番号で引いた名前。Outpost (80AC8635) は 75 式中 **20 式で両者が違い**、従来はそれらを
  「レンダーグローバルに無い」として捨てていた。該当: `sun_intensity` (夜 2 / 正午 35。従来は既定 10 固定)、
  `skybox_sun_intensity` (2 / 348)、`ao_ambient_weight` (0.2。従来 1)、`sun_glow_color` (夜は黒、正午 (0.24,1,0.37)、夕方は赤)、
  `sky_color_override` (夜 (0.053,0.055,0.024) 重み 0.65)、ch1B / ch25 / chB1、cubemap 系 2C5381xx / 2D5382xx、5800C6Fx。
  `MapChannelExpression::channel` は表の名前、`record_id` は元の語。表が無い場合だけ従来どおりレコードの語を使う。
- **シーケンサ入力 (0x4A n)** は、エンティティのもう一方のコンポーネント (Outpost 80AC8638) の Vec<8080AF85>
  `{名前空間, 名前, スロット}` が決める。スロット = 出力数 + n。Outpost: 入力 0 = 6D408B83 (名前空間 "" = シーケンスの
  パラメータ = 時刻)、入力 1 = **rain_amount**、入力 2 = sky_transition_weather、入力 3 = fog_amount
  (名前空間 005921DB = ワールドチャンネル。登録表 80A74014 のスロット 7 / 14 / 8)。
  従来「出どころ不明で 0」としていた入力 1 は rain_amount。`saturate((in1 − 1) / 99)` は雨量 1..100 の正規化で、
  天候状態が無い (雨 0) なら 0 のままが正しい。入力表は式ごとに自分のシーケンスのものを使う (`SequencerInput`)。
- ワールドチャンネル名 (FNV-1 総当たり): 78C38B72 = fog_amount (スロット 8)、324AD09D = wetness_amount (13)、
  56FE36A7 = sky_transition_weather (14)、463C3E52 / 964D8C74 = wind_min_speed / wind_max_speed (9 / 10)。
  天候コントローラー `outpost_ambient` (80B01F4E) が状態ごとに設定する (既定らしい状態 87172882: fog_amount 3..3.5、雨 0)。
  **どの状態が実行時に有効かは未特定**で、nim は状態未指定なら全ワールドチャンネルを 0 とし推定として表示する。
  エンジンの 700 パイプライン / 42 スコープに TFX 0x64 (ワールドチャンネル) の読み手は無い (読むのはマップの材質)。
- レンダーグローバルのチャンネル名 (同じく総当たり): ch01 = autoexposure_bias、ch12 / ch13 = fog_inscatter_lookup_start / _end、
  ch1C = inscatter_intensity、ch64 = sun_light_direction、chAF = sun_inscatter_distance_falloff、
  chBC = fog_noise_intensity、chBD / chBE = fog_noise_height_fade_start / _end。
- 環境エンティティは選択 (バブル / アクティビティ) 内に 1 つだけ (80AC8FFC)。アクティビティ側に別の環境は無い。
  コンポーネントはグループ 23 個 (8080B658 / 8080B65A、根 8080B65C は 3600 / 3600) と出力 75 個の木。
- 未解決: 時刻パラメータ 6D408B83 の実行時の値 (名前も未解読)、天候状態の選択、ACCD238C / 192B0690 / B15E9315 の 3 パラメータ。

### 3.2.4 霧の距離ルックアップと散乱強度 (2026-11-15)
- `fog_density_lookup_start / _end` (ch10 / 11) と `fog_inscatter_lookup_start / _end` (ch12 / 13) は **メートル**。
  TFX は視距離 (m) に対して無変換で使い、全マップの値は Outpost 5..10 / (未設定)、Perimeter 50..100 / 25..75、
  Marsh 50..100 / 5..10、Marathon Deck 10..**10000** / (未設定)。船内の 10000 は km では成立しない。
  従来の「km と読み ×1000」「散乱側は密度側と同じ範囲」は誤りで撤去。
- inscatter CS (80A742F0) は減衰を `lerp(1, saturate((d − start) / (end − start)), extern 0x65 +0x40.y)` 倍、
  太陽散乱を同じ式の第 2 範囲 (extern 7 +0x180 / +0x184 = ch12 / 13) 倍する。+0x40.y は従来 0 (無効)。
  範囲がマップごとに作り込まれていて他に読み手が無いので **1 (推定)**。Deck では 256 m 先でも密度が 2.5% になる。
  未設定 (0, 0) は TFX 上 1/0・0/0。エンジンの扱いは不明なので「start の先は全量」の段差として渡す (推定)。
- extern 7 +0x170 (散乱と空のスケール) = `inscatter_intensity` (ch1C。Outpost は夜 1 → 夕方 0.58)、
  +0x1A0 = `fog_decay_scale` (ch17)。どちらも従来は 1 固定。スロットはシェーダーでの使われ方からの同定 (推定)。
- 密度の単位は再確認済み: 密度 CS の出力は max(fog_density.xy, layered.xy) で正規化、inscatter CS が
  その max × 0.0014427 (= 0.001 / ln2) を掛け直す。/km で正しい。Outpost は (3, 20)/km + ノイズで平均約 9/km。
- 結果 (Outpost 全域、eye (287,−40,21)): 時刻 0.9 で全面のもやが消え、暗い屋外に局所光が分離して見える。
  時刻 0 (空ルックアップは明け方のスライス) は灰色のもやが残り、正午は `sun_glow_color` の緑の散乱が強い。
  **ゲームの実時刻が未特定なので、どの絵がゲームと対応するかは未検証**。露出は上限 (+1 段) に張り付いている。

### 3.7 空の環境光 (実装済み, `src/render/game_sky_ambient.cpp`)
- `cubemap_apply_sky_copy_ao` (80A74C54, PS 80A74C51): 全画面。o0 = 拡散 → ライトバッファ (Deferred +0xC0)、
  o1 = 鏡面 → 鏡面 IBL バッファ (Deferred +0xD0)、o2 = G バッファ AO (捨てる)。キューブマップボリュームより**先**に描き、
  ボリュームが α ブレンドで上書きする。入力: t4/t5 = G バッファ rt2/rt1、**t6 = Deferred +0xF8 = 空の半球マップ**
  (uv = 0.5 + 0.5·normalize(n + ẑ).xy、拡散は mip 6.47、鏡面は粗さ × 6.47)、cb0[12] = ch85, [13] = ch86, [21] = ch80/81,
  [22] = ch54, [27] = ch8C·ch83, [23..26] = **View +0x2A0 = uv(0..1, y 下) → 絶対ワールド** (goriath unk220 = 射影逆行列 × uv→ndc)。
  視線ベクトルは cb12[7] (視点) − その点。
- 半球マップ = z 上向きの鏡面球: テクセル uv → n = (2uv−1, sqrt(1−r²))、方向 = reflect(−ẑ, n)。256² 9 mip (filter_cosine が mip 8 を読む)。
- 生成: 6 面の 90° ビューで `hemisphere_sky_color_generate` (VS: clip = cb0[0..3] (View +0x160, 一時的に単位行列) × (±1, ±1, cb12[13].x, 1)、
  方向 = cb12[4..7] の組 = right·x + up·y + fwd。PS は遠景の空ルックアップ ext7 +0x40/+0x58 を引く) → 128² の面
  → `sky_hemisphere_copy_frustum` (cb12[0..3] = ワールド→クリップ, cb12[7] = 原点、面の視錐台に入る半球テクセルだけ書く。
  t4 = ext 0x26 +0x00、2 つ目のサンプルの回転 cb0[2..4] = ext38 +0xC0..E0 = 単位) → `sky_hemisphere_downsample_filter_ggx`
  (4 タップ ±cb0[4].xy, mip cb0[3].x = ext38 +0xC0 (x = mip, y/z = オフセット)) で mip を作る。
- 推定: 面の解像度 128²、ダウンサンプルのオフセット (ソース半テクセル)。未実装: マップのフレームコピー (地面の照り返し)、
  半球の霧 (frustum_apply_fog)・`sky_hemisphere_copy_and_tint`・SH プローブ CS。`nimshade --no-sky-ambient`、GUI「空の環境光」。
- 結果 (outpost 80AF9550): 日陰の壁が青い空の色で見えるようになった (シーン輝度 0.055 → 0.095)。
- **キューブマップボリュームの黒い箱 (修正済み)**: ボリュームは箱の奥の面で描かれ、箱を通る視線の全画素で PS が走る。
  1) VS は頂点の視深度を dot(視点 − 頂点, cb12[6]) で渡し、PS は表面深度 / それで視線を伸ばして位置を復元する。エンジンの
  cb12[6] はカメラの**後ろ向き**軸だが nim のカメラ空間は +z 前向きなので、復元位置が視点の後ろになっていた →
  ボリュームの VS にだけ cb12[6] を反転したビュースコープを渡す。2) 箱の外を切るのは面フェード
  1 − saturate(|x|·cb11[0] − cb11[2]) だけで、blend 0 の面に 0 を入れていたため切れていなかった → blend 0 = 硬い縁
  (半径の 0.1% で落ちる) にした。結果: 箱の投影全体を暗いプローブで塗る現象が消え、シーン輝度 0.095 → 0.147。
- 残り: CTRL 建物の外壁の一部は屋内用ボリューム (中心 131.7, −38.1, −24.0、プローブ 18×18×8) の内側にあり、焼き込み値が
  0.01〜0.03 (端の列も暗い) なのでそこだけ暗い。データ通りの結果だが、ゲームでそう見えるかは未確認。

### 3.7.1 頭上の遮蔽 (overhead occlusion, 2026-10-18)
ユーザー指示 (原文):「composit_game.pngの壁の一部が暗いけど、この暗さがゲーム本来の色合いだから、こういうライトプローブを探して全部使えるようにして」
- 調査: `nimshade --dump-lighting` + `analyze_probe_coverage.py` で、Outpost の CTRL 視点の表面のうち 313 個の原プローブ箱に入るのは約 22%。
  プローブ箱は総体積 0.69M m³ で建物内・地表付近だけ (最大 97×61×15 グリッド)。2,441 ノードテーブル全 31 コンポーネント型も確認したが、拡散プローブの追加クラスは無い。
  箱の外の拡散光は空の環境光だけで、nim は遮蔽なしの `cubemap_apply_sky_copy_ao` を使っていたため、庇や船体の下でも空の光が全量入り明るすぎた。
- エンジンの変種 **`cubemap_apply_sky_copy_ao_with_overhead_occlusion`** (80A74C5E, PS 80A74C5A) を実装。
  - 入力: Deferred +0x98 (デバイス深度) と View +0x2A0 で画素位置を復元し、法線方向へずらす。それを extern 82 +0x10/+0x20 (xyz: ワールド → (u, v, 標準深度)。permute 1A) で真上からの座標にする。
  - 判定: **extern 82 +0x08 = 分散マップ** (平均深度, 平均深度²) とチェビシェフ不等式で比べる。
  - 遮蔽時: AO 項が AO²·(1 − 0.5·ch 0xB7) になる。**ch 0xB7 の既定値は 1.7** なので、屋根の下では空の光が 15%。
  - 縁: +0x10/+0x20 の .w は縁フェードの uv 上限で、cb0[30] = 0.9·w + 0.05。拡散 o0・鏡面 o1 の両方に掛かる。
- 分散マップはエンジンの **`sample_and_blur_variance_map_9x9`** (CS 80A746EA) をそのまま使う。
  - 処理: 4×4 テクセル → 1 テクセルの (平均, 二乗平均) に縮め、9×9 二項ブラーを掛ける。8×8 グループ。
  - 入力: extern 38 t0 +0x00、+0xD0 = (オフセット, ソースサイズ)・端は折り返し。出力は u0 +0x30。
- ソースの真上からの深度は nim の雨遮蔽マップ (`render_rain_occlusion`: 4096²・標準深度、extern 80 と同じ規約) を流用する。分散マップは 1024² R32G32F。
  - extern 82 +0x00 にマスク、+0x08 に分散マップ、.w = 1 を入れる。エンジン側の頭上遮蔽描画 (範囲・解像度・対象形状) は推定として一覧に出す。
  - シーンの item 数か変換が変わったときだけ作り直す。
- 他の頭上系パイプライン (`render_overhead_visibility_map_*_for_screen`、`copy_texture_compute_overhead_visibility_merge`、extern 81 interior) は未使用。
- 結果 (outpost 標準視点 1280×720): 船体・庇の下の壁と地面が暗くなり、屋根上と日向は不変。シーン輝度は 0.121 → 0.105 (AE が補正)。広域俯瞰 (半径 250 m) でも境界の破綻は無し。
- 頂点焼き込みデータの調査 (同日、結論: **描画には使われていない**):
  - 静的インスタンス変換 (0x60 バイト) の +0x4C は単調増加のバイトオフセットで、最大 98 MB。
    刻みはメッシュの頂点数を 4 B に切り上げた値 (例 80AE8056: 3,201 頂点 → 0xC84 = 3,204 B) なので、**インスタンス × 頂点 1 バイト**の配列を指す。
    D2 の SStaticAmbientOcclusion (頂点 AO) に相当すると推測。ただし識別子 (ヘッダ +0x70、例 84063E08) を含むタグは自分自身だけで、実体のバッファは未発見。
  - `nimcli map-shaders BUBBLE` (追加) で、Outpost の静的 12,975 + 地形 71 が使う 1,742 技法・405 シェーダーの SRV を全件確認した。
    頂点・インスタンス系はすべて t2 (インスタンス 80 B: 行列 + +60 メッシュブロック) と、一部が t0 (メッシュ自身の頂点色、頂点 ID を件数でクランプ) だけ。
    静的 VS の入力も POSITION / TANGENT / TEXCOORD0/1 / VertexID / InstanceID で、AO の入力は無い。
    render globals の全技法でバッファを読むのはスキニング・CMAA・SSR・SDSM・visibility_test・cubemap_dilate・sky irradiance だけで、該当なし。
  - 8080876A (80A79954) は照明ではない。生バッファ 80A7994A/4B (37/31 MB) の先頭はマジック 0xD6000012、格子幅 4.0、マップ AABB (−208,−384,−144)〜(720,512,384)。
    中身は 0x00/0xFF が約 45% を占める。524,288 スロットのハッシュ表と 28 個の AABB、8080876D の 17,258 個の AABB を伴う。
    **4 m ボクセルの空間データ** (可視性・経路・音響などの CPU 側データと推測。Alkahest の SUmbraTomeComponent に当たる Umbra tome の可能性) で、GPU パスの入力にはならない。
- GUI「頭上の遮蔽」、nimshade `--no-overhead-occlusion`。

### 3.7.2 初期カメラ = プレイヤー開始地点 (推定, 2026-10-18)
- ノードコンポーネント **80802D32** (`kClassStartComponent`、`load_map_starts` / `find_map_starts`) はアクティビティのノード表にだけあり、
  ペイロード +0x00 が名前ハッシュ、ノード変換 (+0x00 quat、+0x10 位置) がヨーだけの向きを持つ。
  Marathon Deck (exp) は 3 点ずつの 2 群 (3 人クルー)、Outpost は (148, 52, −4.7) 付近の 4 点、Marsh は 7 点、Perimeter intro は 6 名前 × 2 群。
  スポーン地点と推定 (Marathon のクラス名は未確認。Alkahest SRespawnPoints 80808CB7 は Marathon では globals に 1 件だけ)。
- GUI はマップ読込時 (と F キー) に最初の開始地点の目の高さ 1.7 m (推定) から向き (+X) を見る。範囲を絞った読込もそこを中心にする。
  バブル単体の選択では同じバブルのアクティビティの開始地点を借りる。開始地点が無いマップは配置距離の 80 パーセンタイルで枠取りする
  (以前は最遠の配置 = スカイボックスまでの距離で、全域読込だと数 km 先に飛ばされた)。
- nimshade は `--eye` / `--look` が無いときに同じ開始地点から描画する。`nimcli activities --starts` で一覧。

### 3.8 TAA (実装済み, `src/render/game_post.cpp`)
- `resolve_taa_use_clipping_use_ycocg` (80A74DA3, PS 80A74DA0, VS 80A7430D 全画面)。extern 0x6A: +0x00..+0x0C = cb0[3..6] 履歴の重み、
  +0x10 = cb0[7] 輝度差の閾値、+0x18/+0x1C 入力サイズ (cb0[9])、+0x20/+0x24 解像度 (cb0[14] = これ / View サイズ)、
  t0 = +0x48 履歴、t1 = +0x50 動きベクトル (G バッファ RT3)、t2 = +0x58 現フレーム、t3 = +0x60 デバイス深度。
- PS: 3×3 で最も近い深度の画素の動きベクトルを取り、uv + mv で履歴を Catmull-Rom 9 タップ。現フレームの 3×3 を YCoCg の平均 ± 1.3σ
  にクリップ。w = cb0[4] + (cb0[5] − cb0[4])·mv.z (クリップ前後の輝度差 ≤ cb0[7])、それ以外は lerp(cb0[3], cb0[6], saturate(|mv px|/50)^0.9)。
  出力 = 現フレーム·(1 − w) + 履歴·w。画面端は w = 0。
- **動きベクトルはゲームのシェーダーが書く**: G バッファ PS (例 80A65559) と sky PS が RT3.xy = 0.5·(ndc(View +0x3E0) − ndc(View +0x1A0))、
  y 反転。つまり **View +0x3E0 = 前フレームのワールド → クリップ** (VS cb12[23..26], PS cb12[20..23])。nim は前フレームの行列に今のジッターを
  入れて渡す (静止カメラで動き 0)。ジッターは射影の c[2] (NDC オフセット) に入れ、+0x120/+0x160 の視線行列からは差し引く。
- nim の流れ: トーンマップ → post_ldr_ → resolve_taa (RGBA16F 履歴 2 枚を交互) → copy_texture_bilinear で lit。TAA 中は FXAA を使わない。
  `GameLighting::temporal_view` が毎フレームのジッター付きビューを作る。nimshade は静止画を 16 フレーム回して収束させる (`--no-taa`)。GUI「TAA」。
- 推定: 重み 0.92 / 棄却時 0.75〜0.5、輝度閾値 0.1、ジッター = Halton(2,3) 8 フレーム。未: 他の variant (distort)。sky / stage 8 の SV_Target4 は §3.9 で RT3 に接続済み。

### 3.9 透明物の大気とモーション (2026-10-18)
- 82f6532 の WIP を継承: `load_map_sky_objects` は outpost の 104 配置 / 68 モデルを読み、nimshade は stage 8 を HDR 上に描く。GUI の MapWorker はまだ gbuffer_only のため、空オブジェクトの読込は未統合。
- `atmo_depth_angle_density_lookup_generate` (technique 80A743EF / PS 80A743EC / VS 80A74397) をゲームの DXIL のまま実行。256² RGBA16F (サイズ・形式は推定)。透明スコープ t15 = extern 40 +0x20 に接続。
- PS の入力は SV_Position (TEXCOORD ではない)。extern 7 +0xD0.zw = 1/(サイズ−1)。D = ((pixel.x−0.5)/(W−1))²×50000 m、仰角 = 2×(pixel.y−0.5)/(H−1)−1、評価高さ = eye.z + D×仰角。VS の View +0x160 はこのパスだけ単位行列。既存の近・遠空パスの行列を変更しない。
- フォグの減衰色 = extern 7 +0x190 に ch0x16、+0x1A0 に倍率 1 (推定)。密度・高さ・距離 LUT は不透明物と共通。距離ゼロは透過率 1。
- 透明物の材質・共有定数・SRV は G-buffer 生成時に評価せず、GameLighting 内の透明描画コールバックで、そのフレームの大気 / Transparent / Deferred ブロックから評価。`ExternStore::copy_block_from` は古い参照・定数・ゼロ規則を置換する。G-buffer と影で使用中の材質ディスクリプタは透明物の更新で書き換えない。露出も照明側で更新した値を使い、cb13 は当該パスの共有定数として渡す (次フレームの scene の時計を固定しない)。
- t10 = Deferred +0x98 は、この時点の R32G32 線形深度。SSAO と volumetrics が無効でも透明物には生成する。t25 = extern 40 +0x78 はフロクセル有効時に同一フレームの積分体積 (§3.2.1)、無効時は clear-air (1,1,0,0) を明示的な推定として渡す (null の全ゼロでは透過率も 0 になる)。ライトシャフトスライス等は未接続。
- **モーションの実際のセマンティクスは SV_Target4**。逆コンパイル補助ツールの `o1` は出力リスト上の番号であり、SV_Target1 を意味しない。空と stage 8 の出力4を G-buffer RT3 に接続。中間の MRT 1..3 は null RTV・書込無効。透明物のモーション合成は元の blend state の RT4 を使う。出力4が無い PS は motion write mask 0。
- 空の PS は View +0x1E0 で深度0の点を復元して再投影する。未設定のゼロ行列 / 無限遠射影の逆行列では 0/0 になるため、50km の有限点として復元 (距離は推定として一覧表示)。空・透明物のモーションを finite な値で TAA へ渡す。
- リサイズ時の自前 light diffuse/specular、env、shadow mask の条件付き extern 参照の無効化を `GameLighting::resize` に集約。GUI 以外の呼出元にも適用。旧参照を保持する GPU 検証は対策前に 0xc0000005 で終了し、対策後は大小2回の変更で成功。
- 検証: `ctest --test-dir build -C Release --output-on-failure` (nim_tests + nim_render_tests)。CPU テストはブロック置換時の古いビュー除去・欠落入力のエラー・空の有限点復元を確認。
- GPU 検証: `nimshade 80AF9550 out/verify_frame_inputs --eye 155,-60,-12 --look 125,-28,-26 --map-radius 50 --size 640x360 --verify-frame-inputs`。`--no-sky-objects` を付けた経路も確認。256² の全成分が finite / 0..1、距離0は1。outpost の元密度 LUT は RGBA8 SRGB・全白なので、元 PS の式との最大絶対誤差は 0.00048828125。640×360→657×371→640×360 の最初のフレームから入力サイズが一致、回転時の motion は finite。D3D12 デバッグレイヤーの警告・エラーなし。
- GUI: 起動中の build/Release/nim.exe がロックされるため、`cmake --build build --config Release --target nim -- /p:OutDir=.../out/verified-bin/` で別出力にビルド成功。更新版を outpost 引数で35秒起動し、D3D12 デバッグログは空。専用のテストプロセスは終了済み。マップ切替・リサイズ・タブ閉じの UI 操作は未検証。空の既知不具合が残るため v0.4.0 の正式リリースは未実施。
- **空の合成は未完成**: モデルごとの分離描画で、色の大きな破綻を `80AF906A` (technique 80AF9063 / PS 80AF905F) 単体でも再現。PS alpha の係数 cb0[24].x は `0x64 04` から来る。現 VM は 0x64 を 0x63 と同じ global channel として扱い ch4=6 を渡すため、alpha が 1 を超え得る。これが正しい解釈か、別入力かは未確認。見た目合わせのクランプや任意の色変更は入れていない。以降はこのオペコード / 入力源と空の合成順を解析する。extern 4/5 が未提供の 5 技法も明示的にスキップされる。

## 4. 既知の未解決・注意
- README と `docs/RENDERING.md` 冒頭を現状に更新済み。RENDERING の後半には過去の解析メモが残るため、状態はこの表を優先する。
- 全域の軽量化検証 (Codex): 低温アーカイブ全域 (249,991静的配置、全ライト7369、全原プローブ499) の同一条件で
  一時ディスクリプタ **118,197→2,928 (97.5%減)**、497 unique / 10,538 reused。
  変更前後の320×180画像は全画素一致 (最大差0)。実行全体の所要時間はロードを含むためFPS改善値とは扱わない。
- GUI全域ロードのprepare全スレッドが完了したら、共有CPUテクスチャキャッシュを解放する。キュー済みの
  PreparedMeshは自身のshared_ptrを保持し、アップロード済みGPUテクスチャは残る。全ミップのCPUコピーを
  次のロードまで残していた不要な常駐分を削除 (保持バイト数の実測は未実施)。
- 同一フレーム内で解決済みのビューが完全一致する GamePipeline の SRV/UAV テーブルは共有する (`Gpu::find_transient_table`。キー = スロット・種別・リソース・形式・要素範囲)。フレーム領域の再利用と `transient_rewind` でキャッシュは破棄。nimshade は `view tables: N unique, M reused` を表示。`--dump-lighting` はライトバッファ・深度・プローブ箱を `out/` に出し、`tools/re/analyze_probe_coverage.py` が表面のプローブ箱カバー率を出す (Outpost CTRL 視点で表面の約 22%)。GUI のマップ範囲の既定は「全域」。
- 1 フレームの一時領域 (gpu.hpp): ディスクリプタ 131072 個 / アップロード 128 MB。outpost 全体 (ライト 1300 個描画) で 16k 個 / 12 MB。旧 32768 個では GUI で「per-frame descriptor space exhausted」で描画停止していた。`nimshade` は `frame use` を表示。
- エンティティ (2c12304): GUI の MapWorker でも材質索引・配置を読み、nimshade と同じ dynamic model 準備経路で表示する。配置なしや描画不可のモデルは明示的にスキップ。
- GUI クラッシュ (2026-10-17): outpost `80AF9550` の旧既定「全域」起動は約 31 秒後に `nvwgf2umx.dll` 内の 0xc0000005 で終了した (Windows Application Error 1000)。範囲 50 m の outpost と小さい `80A76433` でも同じアドレスで 4〜7 秒後に再現。ダンプとシンボルで `shading_.draw` の D3D12 SRV 作成と特定し、診断ログで最後の SRV が t12 = Deferred +0xD0 と判明した。ビューポートが 665 px から 1364 px に変わった後も旧サイズの環境テクスチャを指す生ポインターが ExternStore に残っていた。GUI の `gb.resize` 時に Deferred +0xC0/+0xC8/+0xD0 と ShadowMask +0x00/+0x08/+0x10 の参照を無効化して次の描画で再結合する。静的メッシュの extern SRV はフレームごとのリングに変更。当時の既定範囲は 50 m (現在はユーザー指定に従い全域。全域時は約 8 GB の private memory を確認)。修正後の `80A76433` と `80AF9550` は D3D12 デバッグレイヤー付きでそれぞれ 40 秒・50 秒以上継続し、ログは空だった。
- MapWorker の最終キュー要素を完了判定で捨てる不具合を修正。レイアウト読込失敗が worker から未処理例外として出ないようにし、地図アップロード失敗は UI のエラー欄に表示してその読込を停止する。`80AF9550` と `80A76433` は nimshade でエンティティ入りの描画に成功。GUI のマップ切替・リサイズ・タブ閉じの操作検証は未実施。
- 子セッション「Reverse-engineer animation clips and play them」は完了、f86350e で main にマージ済み。
- 2026-11-15: main の作業ツリーに残っていた未コミット作業 (10 月 18 日) を取り込んでコミット。内容: TFX 0x64 を
  レンダーグローバルとは別のワールドチャンネル (52 スロットの登録表) として読む (`ExternStore::world_channels`。
  従来は 0x63 と同じ表を引き、天候がレンダーグローバルの値を上書きしていた)、cubemap の relighting 変種の
  cb11[19..20] に非 relighting 変種と同じ変調になる基準値を入れる (`cubemap_relighting_baseline`。0 のままだと
  焼き込みプローブを消していた)、マップ切替時のプローブ資源の解放、Heat Cascade の判定をチャンネル 798A4399 で行う、
  `nimcli probe-search` / `probe-gpu-audit` / `world-channel-names`、`--map-radius unlimited`、
  `--dump-lighting` に半球マップとチャンネル値。Computer Use は使わずヘッドレスで検証する (AGENTS.md)。
- ゲーム本体は 2026-11-15 に再インストール (Steam build 26462907)。652 ファイル / 158 パッケージ、Outpost のタグは従来と同じ。
