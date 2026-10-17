# 進捗表 / 引き継ぎ (PROGRESS)

**このファイルが進捗の正 (source of truth)。** 他のドキュメント (RENDERING.md など) やコード
コメントと食い違ったら、まずこの表を見て、どちらが新しいかを `git log` で確かめてから直すこと。
作業を進めたら、コミットと同時にこの表を更新する。

最終更新: 2026-10-17 (main 97bf077 の次のコミット)

## 1. 現在のタスク: ゲームのレンダーパイプラインの移植

ユーザー指示 (原文):「ギミックは後にして、ゲームのSSAO・ボリューメトリクス・大気・拡散プローブ・
ポストプロセスのパイプラインを順に移植して」

| # | 項目 | 状態 | コミット / 場所 | 備考 |
|---|------|------|-----------------|------|
| 1 | エンジンパイプラインの取り込み基盤 (GamePipeline, ExternStore, TFX 評価) | 完了 | 〜7ce2de1 | `src/render/game_lighting.cpp` |
| 2 | SSAO (hdao + apply_ssao_to_light_buffers) | 完了 | 97bf077 | §3.1 |
| 3 | 大気 + 空 (sky_lookup_generate_near/far, sky, フォグ) | **作業中** (解析済み・未実装) | — | §3.2。ボリューメトリクスが依存するので順番を入れ替えた |
| 4 | ボリューメトリクス (ライトシャフト) | 解析途中 | — | §3.3。大気/フォグ密度スライスに依存 |
| 5 | 拡散プローブ (light_sample_local_cubemaps_diffuse_probes / sky hemisphere) | 未着手 | — | キューブマップボリュームは 666fc85 で実装済み |
| 6 | ポストプロセス (autoexposure, bloom, LUT/color grading, vignette, final_combine) | 未着手 | — | final_combine だけは既に描画 |
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
  (blend state 3 = DEST_COLOR 乗算) でローカルライトのバッファに掛ける。順序: lights → apply_ssao → cubemaps。
- extern 0x26 は final_combine など全画面パス共通の入力スロット。別パスで使うときは別 ExternStore に入れる。
- 調整値 (半径 10px 等) はエンジン側の値で推定。

### 3.2 大気 + 空 (次に実装するもの)
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
実装計画: `MapAtmosphere load_map_atmosphere(pm, layout)` (content/map) → GameLighting::set_atmosphere
→ 毎フレーム 時刻→スライス座標・太陽方向 (表0) → near/far を全画面 RT に描画 → ext7 +0xE0/+0xF0 →
シェーディング → `sky` を env_bg の代わりに描画。GamePipeline に深度テストのオプションを足す。
GUI: 「時刻」スライダー + 大気トグル。nimshade: `--time T`, `--no-atmosphere`。

### 3.3 ボリューメトリクス (解析途中)
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

## 4. 既知の未解決・注意
- `docs/RENDERING.md` の冒頭 (「未実装」「代替描画を削除」) は古い。現状はこの表が正。
- GUI のフォグ設定 (LightingParams.fog_*) はゲームパスでは未使用。大気実装時に置き換える。
- エンティティ (2c12304) は nimshade のみ。GUI の MapWorker には未統合 (ギミックと一緒に後回し)。
- 子セッション「Reverse-engineer animation clips and play them」は完了、f86350e で main にマージ済み。
