# 進捗表 / 引き継ぎ (PROGRESS)

**このファイルが進捗の正 (source of truth)。** 他のドキュメント (RENDERING.md など) やコード
コメントと食い違ったら、まずこの表を見て、どちらが新しいかを `git log` で確かめてから直すこと。
作業を進めたら、コミットと同時にこの表を更新する。

最終更新: 2026-10-17 (マップ環境チャンネル・ポストプロセス・FXAA のコミット)

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
| 6 | ポストプロセス (autoexposure, bloom, LUT/color grading, FXAA) | 完了 (グレーディング層・vignette は未) | 「Map environment」コミット | §3.5。AE / bloom / トーンマップの設定値は推定 |
| 6b | マップ環境 (太陽・環境光・霧の時刻カーブ = グローバルチャンネル式) | 完了 | 同上 | §3.6 |
| 6c | 空の環境光 (sky hemisphere + cubemap_apply_sky_copy_ao) = 日陰が黒い問題の修正 | 完了 (地面の照り返し・半球の霧/色調は未) | 「Sky ambient」コミット | §3.7 |
| 7 | 検証 (nimshade レンダー比較, テスト), コミット, push, リリース v0.4.0 | 各段で随時 | — | リリース zip は exe と README のみ |

後回し (ユーザー指示で保留): マップのギミック (エンティティの GUI 統合、アニメーション、状態変化)。

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
  SV_Target1 = モーションベクトル (捨ててよい)。深度 = cb12[13].x → 深度テスト GREATER_EQUAL で空だけに描く。
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
  (TAA は前フレーム履歴・ジッター・モーションベクトルが要るので未)。nimshade `--no-aa`、GUI「アンチエイリアス」。

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
- 結果 (outpost 80AF9550): 日陰の壁が青い空の色で見えるようになった (シーン輝度 0.055 → 0.095)。中央の黒い箱は
  屋内用キューブマップボリュームの内側にある可能性 (未調査)。

## 4. 既知の未解決・注意
- `docs/RENDERING.md` の冒頭 (「未実装」「代替描画を削除」) は古い。現状はこの表が正。
- エンティティ (2c12304) は nimshade のみ。GUI の MapWorker には未統合 (ギミックと一緒に後回し)。
- 子セッション「Reverse-engineer animation clips and play them」は完了、f86350e で main にマージ済み。
