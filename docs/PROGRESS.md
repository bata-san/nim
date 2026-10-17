# 進捗表 / 引き継ぎ (PROGRESS)

**このファイルが進捗の正 (source of truth)。** 他のドキュメント (RENDERING.md など) やコード
コメントと食い違ったら、まずこの表を見て、どちらが新しいかを `git log` で確かめてから直すこと。
作業を進めたら、コミットと同時にこの表を更新する。

最終更新: 2026-10-18 (透明物の大気 LUT、当該フレームの入力受け渡し、空・透明物の TAA モーション)

## 1. 現在のタスク: ゲームのレンダーパイプラインの移植

ユーザー指示 (原文):「ギミックは後にして、ゲームのSSAO・ボリューメトリクス・大気・拡散プローブ・
ポストプロセスのパイプラインを順に移植して」

| # | 項目 | 状態 | コミット / 場所 | 備考 |
|---|------|------|-----------------|------|
| 1 | エンジンパイプラインの取り込み基盤 (GamePipeline, ExternStore, TFX 評価) | 完了 | 〜7ce2de1 | `src/render/game_lighting.cpp` |
| 2 | SSAO (hdao + apply_ssao_to_light_buffers) | 完了 | 97bf077 | §3.1 |
| 3 | 大気 + 空 (sky_lookup_generate_near/far, sky, フォグ) | 完了 (フロクセルは未) | 13540ca | §3.2。フロクセル (cascade_raymarch_atmosphere_volume) は後段 |
| 4 | ボリューメトリクス (ライトシャフト) | 完了 (密度スライス・temporal は未) | 142beb4 | §3.3。霧密度スライス (extern 88) の生成元が未特定なので基礎密度のみ = かなり淡い |
| 5 | 拡散プローブ (light_sample_local_cubemaps_diffuse_probes / sky hemisphere) | 完了 (不透明物) | 「Diffuse probes」コミット | §3.4。light_sample_* は透明物/パーティクル用で未 |
| 6 | ポストプロセス (autoexposure, bloom, LUT/color grading, FXAA, TAA) | 完了 (グレーディング層・vignette は未) | 「Map environment」「TAA」コミット | §3.5, §3.8。AE / bloom / トーンマップ / TAA の設定値は推定 |
| 6b | マップ環境 (太陽・環境光・霧の時刻カーブ = グローバルチャンネル式) | 完了 | 同上 | §3.6 |
| 6c | 空の環境光 (sky hemisphere + cubemap_apply_sky_copy_ao) = 日陰が黒い問題の修正 | 完了 (地面の照り返し・半球の霧/色調は未) | 「Sky ambient」「Cubemap volume bounds」コミット | §3.7 (箱の境界も修正) |
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
未: フロクセル t14 (no_atmosphere のまま)、t2 パラボロイド (白)、太陽強度の時刻変化 (ビューア値)。
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
- フロクセル (t14) は `cascade_raymarch_atmosphere_volume*` (CS, extern 0x65) が作る。後段の課題。
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

### 3.4 拡散プローブ + マップのライティング (実装済み)
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
- t10 = Deferred +0x98 は、この時点の R32G32 線形深度。SSAO と volumetrics が無効でも透明物には生成する。t25 = extern 40 +0x78 は未実装フロクセルの clear-air (1,1,0,0) を明示的な推定として渡す (null の全ゼロでは透過率も 0 になる)。ライトシャフトスライス等は未接続。
- **モーションの実際のセマンティクスは SV_Target4**。逆コンパイル補助ツールの `o1` は出力リスト上の番号であり、SV_Target1 を意味しない。空と stage 8 の出力4を G-buffer RT3 に接続。中間の MRT 1..3 は null RTV・書込無効。透明物のモーション合成は元の blend state の RT4 を使う。出力4が無い PS は motion write mask 0。
- 空の PS は View +0x1E0 で深度0の点を復元して再投影する。未設定のゼロ行列 / 無限遠射影の逆行列では 0/0 になるため、50km の有限点として復元 (距離は推定として一覧表示)。空・透明物のモーションを finite な値で TAA へ渡す。
- リサイズ時の自前 light diffuse/specular、env、shadow mask の条件付き extern 参照の無効化を `GameLighting::resize` に集約。GUI 以外の呼出元にも適用。旧参照を保持する GPU 検証は対策前に 0xc0000005 で終了し、対策後は大小2回の変更で成功。
- 検証: `ctest --test-dir build -C Release --output-on-failure` (nim_tests + nim_render_tests)。CPU テストはブロック置換時の古いビュー除去・欠落入力のエラー・空の有限点復元を確認。
- GPU 検証: `nimshade 80AF9550 out/verify_frame_inputs --eye 155,-60,-12 --look 125,-28,-26 --map-radius 50 --size 640x360 --verify-frame-inputs`。`--no-sky-objects` を付けた経路も確認。256² の全成分が finite / 0..1、距離0は1。outpost の元密度 LUT は RGBA8 SRGB・全白なので、元 PS の式との最大絶対誤差は 0.00048828125。640×360→657×371→640×360 の最初のフレームから入力サイズが一致、回転時の motion は finite。D3D12 デバッグレイヤーの警告・エラーなし。
- GUI: 起動中の build/Release/nim.exe がロックされるため、`cmake --build build --config Release --target nim -- /p:OutDir=.../out/verified-bin/` で別出力にビルド成功。更新版を outpost 引数で35秒起動し、D3D12 デバッグログは空。専用のテストプロセスは終了済み。マップ切替・リサイズ・タブ閉じの UI 操作は未検証。空の既知不具合が残るため v0.4.0 の正式リリースは未実施。
- **空の合成は未完成**: モデルごとの分離描画で、色の大きな破綻を `80AF906A` (technique 80AF9063 / PS 80AF905F) 単体でも再現。PS alpha の係数 cb0[24].x は `0x64 04` から来る。現 VM は 0x64 を 0x63 と同じ global channel として扱い ch4=6 を渡すため、alpha が 1 を超え得る。これが正しい解釈か、別入力かは未確認。見た目合わせのクランプや任意の色変更は入れていない。以降はこのオペコード / 入力源と空の合成順を解析する。extern 4/5 が未提供の 5 技法も明示的にスキップされる。

## 4. 既知の未解決・注意
- README と `docs/RENDERING.md` 冒頭を現状に更新済み。RENDERING の後半には過去の解析メモが残るため、状態はこの表を優先する。
- 1 フレームの一時領域 (gpu.hpp): ディスクリプタ 131072 個 / アップロード 128 MB。outpost 全体 (ライト 1300 個描画) で 16k 個 / 12 MB。旧 32768 個では GUI で「per-frame descriptor space exhausted」で描画停止していた。`nimshade` は `frame use` を表示。
- エンティティ (2c12304): GUI の MapWorker でも材質索引・配置を読み、nimshade と同じ dynamic model 準備経路で表示する。配置なしや描画不可のモデルは明示的にスキップ。
- GUI クラッシュ (2026-10-17): outpost `80AF9550` の旧既定「全域」起動は約 31 秒後に `nvwgf2umx.dll` 内の 0xc0000005 で終了した (Windows Application Error 1000)。範囲 50 m の outpost と小さい `80A76433` でも同じアドレスで 4〜7 秒後に再現。ダンプとシンボルで `shading_.draw` の D3D12 SRV 作成と特定し、診断ログで最後の SRV が t12 = Deferred +0xD0 と判明した。ビューポートが 665 px から 1364 px に変わった後も旧サイズの環境テクスチャを指す生ポインターが ExternStore に残っていた。GUI の `gb.resize` 時に Deferred +0xC0/+0xC8/+0xD0 と ShadowMask +0x00/+0x08/+0x10 の参照を無効化して次の描画で再結合する。静的メッシュの extern SRV はフレームごとのリングに変更。既定範囲は 50 m (全域時は約 8 GB の private memory を確認)。修正後の `80A76433` と `80AF9550` は D3D12 デバッグレイヤー付きでそれぞれ 40 秒・50 秒以上継続し、ログは空だった。
- MapWorker の最終キュー要素を完了判定で捨てる不具合を修正。レイアウト読込失敗が worker から未処理例外として出ないようにし、地図アップロード失敗は UI のエラー欄に表示してその読込を停止する。`80AF9550` と `80A76433` は nimshade でエンティティ入りの描画に成功。GUI のマップ切替・リサイズ・タブ閉じの操作検証は未実施。
- 子セッション「Reverse-engineer animation clips and play them」は完了、f86350e で main にマージ済み。
