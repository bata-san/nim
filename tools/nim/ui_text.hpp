// nim's own interface text (not game content) in Japanese, English and Korean.
//
// Every string the GUI shows is one row of NIM_UI_STRINGS: the key, then the Japanese (the default),
// English and Korean text. Printf-style rows (%s, %zu, ...) are used with ImGui calls through tr();
// std::format-style rows ({}, {:.1f}, ...) are used through trf(). Keep the placeholders the same in
// every language of a row.
//
// Game content (string-table text, subtitles, speaker names, the 13 content-language names in the
// content selectors) is not in this table; it keeps its own language selectors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace nim::ui {

enum class Lang : uint8_t { Ja, En, Ko, Count };

// X(key, japanese, english, korean)
#define NIM_UI_STRINGS(X)                                                                                              \
    X(AppSubtitle, "Marathon アセットビューア", "Marathon asset viewer", "Marathon 에셋 뷰어")                              \
    X(NavFind, "検索", "Search", "검색") \
    X(DescFind, "マップ・台詞・コデックス・ムービー・テキストをまとめて検索", "Search maps, dialogue, codex, movies and text at once", "맵·대사·코덱스·영상·텍스트를 한 번에 검색") \
    X(PromptHint, "検索…", "Search…", "검색…") \
    X(FindEmpty, "上の検索欄 (Ctrl+K) にキーワードを入れて Enter", "Type a keyword in the search box above (Ctrl+K) and press Enter", "위 검색창 (Ctrl+K) 에 키워드를 넣고 Enter") \
    X(FindHits, "{} 件", "{} hits", "{}건") \
    X(FindOpen, "このページで絞り込む", "Filter this page", "이 페이지에서 걸러 보기") \
    X(FindNone, "見つかりません: {}", "Nothing found: {}", "찾지 못했습니다: {}") \
    X(MapKindMap, "マップ単体", "Map only", "맵 단독") \
    X(MapKindMode, "ゲームモード", "Game mode", "게임 모드") \
    X(NavHome, "ホーム", "Home", "홈") \
    X(NavStatics, "背景物", "Scenery", "배경물") \
    X(NavEntities, "モデル", "Models", "모델") \
    X(NavMaps, "マップ", "Maps", "맵") \
    X(NavText, "テキスト", "Text", "텍스트") \
    X(NavDialogue, "台詞", "Dialogue", "대사") \
    X(NavMovies, "ムービー", "Movies", "영상") \
    X(NavCodex, "コデックス", "Codex", "코덱스") \
    X(NavPackages, "パッケージ", "Packages", "패키지") \
    X(NavLog, "ログ", "Log", "로그") \
    X(NavSettings, "設定", "Settings", "설정") \
    X(GroupModels3D, "3D ビュー", "3D views", "3D 보기")                                                                                    \
    X(Group2D, "テキストとメディア", "Text and media", "텍스트·미디어")                                                                                          \
    X(GroupData, "データとアプリ", "Data and app", "데이터·앱")                                                                              \
    X(DescHome, "Marathon のアセットを 3D と 2D から閲覧します", "Browse Marathon's assets in 3D and 2D",                  \
      "Marathon 에셋을 3D와 2D로 살펴봅니다")                                                                              \
    X(DescStatics, "建物・地形・小物などの背景物を 3D サムネイルで一覧し、ゲームのシェーダで表示", "Buildings, terrain and props, listed as live 3D thumbnails and drawn with the game shaders", "건물·지형·소품을 3D 썸네일로 보고 게임 셰이더로 표시") \
    X(DescEntities, "キャラクター・装備・機械を、材質とアニメーション付きで表示", "Characters, gear and machines with their materials and animations", "캐릭터·장비·기계를 재질과 애니메이션으로 표시") \
    X(DescMaps, "マップを丸ごと読み込んで歩き回る (時刻・天候つき)", "Load a whole map and walk through it, with its time of day and weather", "맵 전체를 불러와 돌아다닙니다 (시간·날씨 포함)") \
    X(DescText, "ゲーム内の文字列を 13 言語で検索", "Search the game text in 13 languages", "게임 문자열을 13개 언어로 검색") \
    X(DescDialogue, "台詞を全言語で表示し、音声を再生・話者ごとに書き出す",                                              \
      "Lines in every language, with voice playback and per-speaker WAV export",                                           \
      "모든 언어의 대사를 보여주고, 음성 재생과 화자별 WAV 내보내기를 합니다")                                              \
    X(DescMovies, "ムービー (USM) を音声トラック・字幕の言語を選んで再生",                                              \
      "Play USM movies with a chosen audio track and subtitle language",                                                   \
      "USM 영상을 음성 트랙과 자막 언어를 골라 재생합니다")                                                                 \
    X(DescCodex, "ゲーム内コデックスの記録を全言語で読み、音声ログを再生", "Read the in-game codex records in every language and play their audio logs", "게임 내 코덱스 기록을 모든 언어로 읽고 음성 로그를 재생합니다") \
    X(DescPackages, "インストールのファイルとタグの生データ (開発向け)", "The install files and raw tag bytes (for development)", "설치 파일과 태그 원시 데이터 (개발용)") \
    X(DescLog, "読込とエラーの記録", "Load and error messages", "로드 및 오류 기록")                                       \
    X(DescSettings, "インストール・表示・言語", "Install, display and language", "설치·표시·언어")                          \
    X(PickInstallFolder, "Marathon のインストールフォルダを選択", "Choose the Marathon install folder",                  \
      "Marathon 설치 폴더 선택")                                                                                           \
    X(Preparing, "準備中…", "Preparing…", "준비 중…")                                                                     \
    X(Loading, "読込中…", "Loading…", "불러오는 중…")                                                                     \
    X(StatusListingMeshes, "静的メッシュを列挙中…", "Listing static meshes…", "정적 메시 목록 작성 중…")                    \
    X(StatusLoadingText, "テキストを読込中…", "Loading text…", "텍스트 불러오는 중…")                                       \
    X(StatusLoadingDialogue, "セリフを読込中…", "Loading dialogue…", "대사 불러오는 중…")                                    \
    X(StatusLoadingCodex, "コデックスを読込中…", "Loading the codex…", "코덱스 불러오는 중…") \
    X(StatusListingMovies, "ムービーを列挙中…", "Listing movies…", "영상 목록 작성 중…")                                    \
    X(StatusLoadError, "読込エラー: {}", "Load error: {}", "불러오기 오류: {}")                                           \
    X(MeshesListing, "メッシュを列挙中…", "Listing meshes…", "메시 목록 작성 중…")                                        \
    X(MapsListing, "マップを列挙中…", "Listing maps…", "맵 목록 작성 중…")                                                \
    X(ErrMapList, "マップを列挙できません: {}", "Cannot list maps: {}", "맵 목록을 만들 수 없습니다: {}")                  \
    X(MapsEmpty, "静的ジオメトリを持つマップがありません", "No maps with static geometry",                              \
      "정적 지오메트리가 있는 맵이 없습니다")                                                                              \
    X(NoInstallTitle, "Marathon が見つかりません", "Marathon not found", "Marathon을 찾을 수 없습니다")                    \
    X(NoInstallLine1, "Steam のインストールを自動で見つけられませんでした。",                                           \
      "The Steam install could not be found automatically.", "Steam 설치 위치를 자동으로 찾지 못했습니다.")                  \
    X(NoInstallLine2, "Marathon のフォルダ (Marathon.exe と packages があるところ) を選んでください。",                \
      "Choose the Marathon folder (the one with Marathon.exe and packages).",                                              \
      "Marathon.exe와 packages가 있는 Marathon 폴더를 선택하세요.")                                                         \
    X(SelectFolder, "フォルダを選ぶ", "Choose folder", "폴더 선택")                                                      \
    X(ErrNoSteamInstall, "Marathon の Steam インストールが見つかりません。設定からフォルダを選んでください。",              \
      "Marathon's Steam install was not found. Choose the folder in Settings.",                                            \
      "Marathon의 Steam 설치를 찾지 못했습니다. 설정에서 폴더를 고르세요.")                                                  \
    X(NoInstallSelected, "インストール未選択", "No install selected", "설치 폴더 미선택")                                  \
    X(OpenInstallHint, "Marathon のインストールを開いてください", "Open a Marathon install first",                        \
      "먼저 Marathon 설치 폴더를 여세요")                                                                                  \
    X(ClickCardHint, "カードをクリックすると、ゲームのシェーダで描画します",                                              \
      "Click a card to draw it with the game's shaders", "카드를 클릭하면 게임 셰이더로 그립니다")                           \
    X(NothingSelected, "メッシュ未選択", "Nothing selected", "선택된 메시 없음")                                           \
    X(PickMovie, "左の一覧からムービーを選ぶと再生します。", "Choose a movie from the list on the left to play it.",      \
      "왼쪽 목록에서 영상을 고르면 재생합니다.")                                                                             \
    X(LoadingItem, "{} を読込中…", "Loading {}…", "{} 불러오는 중…")                                                      \
    X(SidebarHide, "サイドバーを畳む (Ctrl+B)", "Collapse the sidebar (Ctrl+B)", "사이드바 접기 (Ctrl+B)") \
    X(SidebarShow, "サイドバーを開く (Ctrl+B)", "Expand the sidebar (Ctrl+B)", "사이드바 펼치기 (Ctrl+B)") \
    X(HomeInstall, "インストール", "Install", "설치")                                                                    \
    X(HomeChange, "変更…", "Change…", "변경…")                                                                           \
    X(HomeFooter, "nim {}  —  Marathon アセットビューア", "nim {}  —  Marathon asset viewer",                          \
      "nim {}  —  Marathon 에셋 뷰어")                                                                                     \
    X(SettingsInstall, "インストール", "Install", "설치")                                                                \
    X(SettingsNoInstall, "(未選択)", "(none)", "(없음)")                                                                 \
    X(SettingsSelectFolder, "フォルダを選択…", "Choose folder…", "폴더 선택…")                                            \
    X(SettingsLanguage, "言語", "Language", "언어")                                                                      \
    X(SettingsUiLanguage, "表示言語", "Interface language", "인터페이스 언어")                                            \
    X(SettingsLanguageNote, "表示言語はこの画面の文字だけを変えます。ゲームの文字・字幕・話者名は各タブの言語で選びます。",       \
      "The interface language changes this app's text only. Game text, subtitles and speaker names use the language selectors in each tab.", \
      "표시 언어는 이 앱의 문자만 바꿉니다. 게임 텍스트·자막·화자 이름은 각 탭의 언어 선택으로 정합니다.")                 \
    X(SettingsDisplay, "表示", "Display", "표시")                                                                        \
    X(SettingsShaders, "ゲームのシェーダのみ (代替描画なし)", "Game shaders only (no substitute rendering)",             \
      "게임 셰이더만 사용 (대체 렌더링 없음)")                                                                              \
    X(SettingsThumbnails, "3D サムネイルを生成", "Generate 3D thumbnails", "3D 썸네일 생성")                              \
    X(SettingsShowFps, "FPS / VRAM を表示", "Show FPS / VRAM", "FPS / VRAM 표시")                                       \
    X(SettingsSecondLang, "第 2 言語 (ゲームの文字)", "Second language (game text)", "두 번째 언어 (게임 텍스트)")          \
    X(SettingsInfo, "情報", "About", "정보")                                                                            \
    X(SettingsAutoUpdate, "起動時に更新を確認", "Automatically check for updates", "시작 시 업데이트 확인") \
    X(CheckUpdates, "今すぐ更新を確認", "Check for updates now", "지금 업데이트 확인") \
    X(UpdateAuth, "更新に GitHub 認証が必要です。", "GitHub authentication is required for updates.", "업데이트에 GitHub 인증이 필요합니다.") \
    X(UpdateReady, "更新を取得しました。nim の終了後に適用します。", "Update downloaded. It will install when nim exits.", "nim을 종료하면 업데이트가 설치됩니다.") \
    X(UpdateRestart, "再起動して更新を適用", "Restart to apply update", "재시작하여 업데이트 적용") \
    X(UpdateCurrent, "最新版を使用しています。", "You are on the latest version.", "최신 버전을 사용 중입니다.") \
    X(UpdateChecking, "更新を確認しています。", "Checking for updates.", "업데이트를 확인 중입니다.") \
    X(UpdateUnavailable, "更新できませんでした。", "Could not update nim.", "nim을 업데이트할 수 없습니다.") \
    X(SettingsGpu, "GPU: {}", "GPU: {}", "GPU: {}")                                                                      \
    X(SettingsVram, "VRAM: {} / {}", "VRAM: {} / {}", "VRAM: {} / {}")                                                   \
    X(SettingsFonts, "フォント: {}", "Fonts: {}", "글꼴: {}")                                                            \
    X(FontsGame, "ゲームのフォント (インストールから読込)", "Game fonts (loaded from the install)",                      \
      "게임 글꼴 (설치에서 불러옴)")                                                                                       \
    X(FontsSystem, "システムフォント", "System fonts", "시스템 글꼴")                                                    \
    X(SettingsThumbs, "サムネイル: {} 枚", "Thumbnails: {}", "썸네일: {}개")                                              \
    X(Estimated, "推定", "Estimated", "추정")                                                                            \
    X(EstimatesHeader, "推定値 ({})", "Estimated values ({})", "추정값 ({})")                                             \
    X(EstimatesHint, "ゲームデータが無い入力 (マップ未読込・実行時の状態) に nim が入れた値",                             \
      "Inputs with no game data behind them (no map loaded, run-time state) that nim filled in",                          \
      "게임 데이터가 없는 입력(맵 미로드, 실행 중 상태)에 nim이 넣은 값")                                                    \
    X(ChannelsHeader, "オブジェクトの状態 (チャネル {})", "Object state (channels {})", "오브젝트 상태 (채널 {})")          \
    X(ChannelsHint, "マテリアルが読む値 (氷結・ヒートカスケード・ダメージ・ディゾルブなど)。名前が不明なものはハッシュ表示",   \
      "Values the materials read (freeze, heat cascade, damage, dissolve ...). Unknown names show their hash.",            \
      "머티리얼이 읽는 값(동결·열 캐스케이드·피해·디졸브 등). 이름을 모르면 해시로 표시합니다.")                               \
    X(ColChannel, "チャネル", "Channel", "채널")                                                                         \
    X(ColStage, "段", "Stage", "단계")                                                                                   \
    X(ColValue, "値", "Value", "값")                                                                                     \
    X(ResetDefault, "既定値に戻す", "Reset to default", "기본값으로 되돌리기")                                             \
    X(Default, "既定", "Default", "기본")                                                                                \
    X(Copy, "コピー", "Copy", "복사")                                                                                    \
    X(Play, "再生", "Play", "재생")                                                                                      \
    X(Pause, "一時停止", "Pause", "일시정지")                                                                            \
    X(Rewind, "先頭に戻す", "Rewind", "처음으로")                                                                       \
    X(Audio, "音声", "Audio", "음성")                                                                                    \
    X(Voice, "音声", "Voice", "음성")                                                                                    \
    X(VoiceFollowText, "本文の言語に合わせる", "Match text language", "텍스트 언어에 맞춤")                                  \
    X(None, "なし", "None", "없음")                                                                                      \
    X(Off, "オフ", "Off", "끔")                                                                                          \
    X(Unsupported, "未対応", "unsupported", "미지원")                                                                    \
    X(Multilingual, "多言語", "Multilingual", "다국어")                                                                  \
    X(Subtitles, "字幕", "Subtitles", "자막")                                                                            \
    X(Filter, "絞り込み", "Filter", "필터")                                                                              \
    X(Export, "書き出し…", "Export…", "내보내기…")                                                                       \
    X(Abort, "中止", "Stop", "중지")                                                                                     \
    X(Caption, "キャプション", "Caption", "캡션")                                                                        \
    X(ColName, "名前", "Name", "이름")                                                                                   \
    X(ColTag, "タグ", "Tag", "태그")                                                                                     \
    X(ColType, "種別", "Type", "종류")                                                                                   \
    X(ColClass, "クラス", "Class", "클래스")                                                                             \
    X(ColSize, "サイズ", "Size", "크기")                                                                                 \
    X(ColTime, "時刻", "Time", "시각")                                                                                   \
    X(ColHash, "ハッシュ", "Hash", "해시")                                                                               \
    X(ColLanguage, "言語", "Language", "언어")                                                                           \
    X(ColSpeaker, "話者", "Speaker", "화자")                                                                             \
    X(ColLine, "台詞", "Line", "대사")                                                                                   \
    X(ColTable, "テーブル", "Table", "테이블")                                                                           \
    X(ColTexture, "テクスチャ", "Texture", "텍스처")                                                                     \
    X(ColFormat, "形式", "Format", "형식")                                                                               \
    X(ColEnglishKey, "英語 / キー", "English / key", "영어 / 키")                                                        \
    X(SortLabel, "並び順", "Sort", "정렬")                                                                               \
    X(SortName, "名前", "Name", "이름")                                                                                  \
    X(SortCount, "数", "Count", "개수")                                                                                  \
    X(Bytes, "%zu バイト", "%zu bytes", "%zu 바이트")                                                                    \
    X(Pick, "選ぶ", "Pick", "선택")                                                                                      \
    X(LoadAtCamera, "カメラの位置で読込", "Load at camera", "카메라 위치에서 불러오기")                                   \
    X(LoadAtCameraTip, "いまのカメラの注視点を中心に、範囲内を読み込み直します",                                         \
      "Reloads the area around the current camera target", "현재 카메라 주시점을 중심으로 범위를 다시 불러옵니다")           \
    X(RadiusUnlimited, "範囲  無制限", "Radius  unlimited", "범위  무제한")                                              \
    X(RadiusFmt, "範囲  %.0f m", "Radius  %.0f m", "범위  %.0f m")                                                      \
    X(RadiusTip, "領域の中心からこの距離にある配置を読み込みます (右端 = マップ全域)",                                     \
      "Loads placements within this distance of the region centre (rightmost = whole map)",                              \
      "영역 중심에서 이 거리 안의 배치를 불러옵니다 (오른쪽 끝 = 맵 전체)")                                                  \
    X(TexCap256, "テクスチャ  256", "Texture  256", "텍스처  256")                                                      \
    X(TexCap512, "テクスチャ  512", "Texture  512", "텍스처  512")                                                      \
    X(TexCap1024, "テクスチャ  1024", "Texture  1024", "텍스처  1024")                                                  \
    X(TexCapFull, "テクスチャ  原寸", "Texture  full size", "텍스처  원본 크기")                                          \
    X(TexCapTip, "テクスチャの最大辺 (VRAM と読込時間の節約。原寸 = ゲームのデータそのまま)",                              \
      "Longest texture edge (saves VRAM and load time; full size = the game's data as is)",                              \
      "텍스처 최대 변 (VRAM과 로드 시간 절약. 원본 = 게임 데이터 그대로)")                                                  \
    X(MapKindTip, "マップ = マップ単体 / モード = 遊ぶモードが使うマップ (モード固有の配置と状態を含む)", "map = the map alone / mode = the map as a game mode uses it (with the mode's own placements and state)", "맵 = 맵 단독 / 모드 = 게임 모드가 쓰는 맵 (모드 고유 배치와 상태 포함)") \
    X(MapFilterHint, "  マップ / タグ", "  Map / tag", "  맵 / 태그")                                                  \
    X(MapRowStats, "{}   {} メッシュ · {} 配置", "{}   {} meshes · {} placements", "{}   메시 {}개 · 배치 {}개")          \
    X(MapActivityRow, "モード {} / 区画 {} → {}", "mode {} / part {} → {}", "모드 {} / 구역 {} → {}") \
    X(EventPalette, "イベントパレット", "Event palette", "이벤트 팔레트") \
    X(EventFilter, "イベントを絞り込む", "Filter events", "이벤트 필터") \
    X(EventIdle, "通常状態", "Idle map", "기본 상태") \
    X(EventPreviewHint, "選択したイベントを手動で再生します。", "Play the selected event on demand.", "선택한 이벤트를 수동으로 재생합니다.") \
    X(EventPlay, "再生", "Play", "재생") \
    X(EventReplay, "最初から再生", "Replay", "다시 재생") \
    X(EventStop, "停止", "Stop", "정지") \
    X(EventStaticOnly, "配置のみ表示できます", "Placement preview only", "배치만 미리 볼 수 있습니다") \
    X(EventAuto, "近づいたら発動", "Start on approach", "접근 시 발동") \
    X(EventAutoTip, "敵 AI をオンにします。イベントに属する分隊はそのイベントで出現し、近づくと発動します (降下艇は 60 m、ほかは 30 m、どちらも推定)。イベントに属さない分隊は AI の距離で出現します。イベントの FX・モデル・音も近づくと再生し、離れると止まります。パレットから再生したイベントは停止するまで続きます。", "Turns the enemy AI on. An event's squads come with the event, which fires as you approach (dropships 60 m, others 30 m; estimates); other squads spawn by the AI's distance rules. Event FX, models and sounds also play on approach and stop when you leave; one played from the palette runs until stopped.", "적 AI를 켭니다. 이벤트에 속한 분대는 그 이벤트로 출현하며, 접근하면 발동합니다 (강하선 60 m, 그 외 30 m, 모두 추정). 이벤트에 속하지 않은 분대는 AI의 거리 규칙으로 출현합니다. 이벤트의 FX·모델·소리도 접근하면 재생되고 멀어지면 멈춥니다. 팔레트에서 재생한 이벤트는 정지할 때까지 계속됩니다.") \
    X(EventSquads, "  [敵 {} 分隊]", "  [{} enemy squads]", "  [적 {} 분대]") \
    X(EventNoSquad, "  [分隊なし]", "  [no squad]", "  [분대 없음]") \
    X(EventDropshipHint, "敵 AI の降下艇として呼び出します", "Calls the enemy AI's dropship", "적 AI 강하선으로 호출합니다") \
    X(LogEventSquads, "イベント {}: 分隊 {} 個 ({} 体) を出現させました", "event {}: brought {} squad(s) ({} members)", "이벤트 {}: 분대 {}개 ({}명)를 출현시켰습니다") \
    X(LogEventNoSquad, "イベント {}: 今回の AI にこのイベントの分隊がいません (選ばれなかった代替分隊、または分隊のいない地点)", "event {}: no squad of this AI run belongs to it (an alternative left out, or nodes no squad uses)", "이벤트 {}: 이번 AI에 이 이벤트의 분대가 없습니다 (선택되지 않은 대체 분대이거나 분대가 없는 지점)") \
    X(MapLoading, "読込中  %zu / %zu", "Loading  %zu / %zu", "불러오는 중  %zu / %zu")                                    \
    X(MapCounts, "%zu メッシュ · %zu 配置", "%zu meshes · %zu placements", "메시 %zu개 · 배치 %zu개")                      \
    X(MapCenter, "中心 (%.0f, %.0f, %.0f)", "Centre (%.0f, %.0f, %.0f)", "중심 (%.0f, %.0f, %.0f)")                      \
    X(MapProbes, "原プローブ %zu 領域 / 描画 %zu", "Original probes: %zu areas / %zu drawn",                           \
      "원본 프로브 %zu개 영역 / 그린 수 %zu")                                                                              \
    X(MapEmptyHint, "マップを選ぶと、周辺の静的ジオメトリをゲームのシェーダで描画します",                                 \
      "Choose a map to draw its nearby static geometry with the game's shaders",                                          \
      "맵을 고르면 주변 정적 지오메트리를 게임 셰이더로 그립니다")                                                          \
    X(FlyHint, "右ドラッグ + WASD / QE で移動 (Shift 速く · Ctrl 遅く · ホイールで速度)",                                 \
      "Right-drag + WASD / QE to move (Shift faster · Ctrl slower · wheel for speed)",                                    \
      "오른쪽 드래그 + WASD / QE로 이동 (Shift 빠르게 · Ctrl 느리게 · 휠로 속도)")                                          \
    X(MapViewLine, "マップ  %s", "map  %s", "맵  %s") \
    X(ActivityViewLine, "モード %s / 区画 %u → %s", "mode %s / part %u → %s", "모드 %s / 구역 %u → %s") \
    X(SkippedHeader, "スキップしたメッシュ ({})", "Skipped meshes ({})", "건너뛴 메시 ({})")                              \
    X(MapUnsupported, "未対応: マップの大気 / 空・雨や VFX のパーティクル・ギミックの動き (推定値は下に記載)",               \
      "Not yet supported: map atmosphere and sky, rain and VFX particles, gimmick motion (estimates listed below)",       \
      "미지원: 맵 대기·하늘, 비와 VFX 파티클, 기믹 동작 (추정값은 아래 표시)")                                             \
    X(HeatCascade, "ヒートカスケード", "Heat cascade", "열 캐스케이드")                                                  \
    X(HeatCascadeTip, "%s\n%s\n状態の値は範囲の中央 (推定)。対応する元のマテリアルに適用。",                             \
      "%s\n%s\nState values use the middle of their range (estimated). Applied to the matching original materials.",     \
      "%s\n%s\n상태 값은 범위의 가운데 값 (추정). 대응하는 원본 머티리얼에 적용합니다.")                                     \
    X(HeatCascadeShared, "Outpost の元の状態を共用 (他マップへの適用は推定)",                                            \
      "Shares Outpost's original state (applying it to other maps is estimated)",                                         \
      "Outpost의 원본 상태를 공유 (다른 맵 적용은 추정)")                                                                    \
    X(HeatCascadeOwn, "このマップの元の状態", "This map's own original state", "이 맵의 원본 상태")                      \
    X(HeatCascadeMissing, "インストール内に元のヒートカスケード状態が見つかりません",                                    \
      "The original heat-cascade state was not found in the install", "설치에서 원본 열 캐스케이드 상태를 찾지 못했습니다")      \
    X(WeatherHeader, "天候とワールドチャンネル ({})", "Weather and world channels ({})", "날씨와 월드 채널 ({})")           \
    X(WeatherEstimate, "状態の値は範囲の中央 (推定: ゲームは範囲内で選び、遷移時間で補間)",                               \
      "State values use the middle of the range (estimated: the game picks within the range and blends over time)",      \
      "상태 값은 범위의 가운데 값 (추정: 게임은 범위 안에서 고르고 전환 시간으로 보간)")                                      \
    X(LogMapLoadAll, "マップ {} ({}) を読込: 全域", "Loaded map {} ({}): whole map", "맵 {} ({}) 불러옴: 전체")            \
    X(LogMapLoadRadius, "マップ {} ({}) を読込: 半径 {:.0f} m", "Loaded map {} ({}): radius {:.0f} m",                   \
      "맵 {} ({}) 불러옴: 반경 {:.0f} m")                                                                                  \
    X(LogCameraStart, "カメラ: プレイヤー開始地点 (推定, 80802D32) ({:.1f}, {:.1f}, {:.1f}) + 目の高さ {:.1f} m (推定)",   \
      "Camera: player start (estimated, 80802D32) ({:.1f}, {:.1f}, {:.1f}) + eye height {:.1f} m (estimated)",            \
      "카메라: 플레이어 시작 지점 (추정, 80802D32) ({:.1f}, {:.1f}, {:.1f}) + 눈높이 {:.1f} m (추정)")                       \
    X(LogMapDone, "マップ {}: {} メッシュ / {} インスタンス{}", "Map {}: {} meshes / {} instances{}",                     \
      "맵 {}: 메시 {}개 / 인스턴스 {}개{}")                                                                                \
    X(LogSkipped, " ({} スキップ)", " ({} skipped)", " ({}개 건너뜀)")                                                   \
    X(ErrMapStopped, "マップの読込を停止: {}", "Map loading stopped: {}", "맵 불러오기 중지: {}")                         \
    X(AnimationTab, "アニメーション", "Animation", "애니메이션") \
    X(ParametersTab, "パラメータ", "Parameters", "매개변수") \
    X(ModelInfoTab, "モデル情報", "Model info", "모델 정보") \
    X(AnimationInfo, "リグ・クリップ情報", "Rig and clip info", "리그 및 클립 정보") \
    X(ClipFilter, "クリップタグ / 名前ハッシュ", "Clip tag / name hash", "클립 태그 / 이름 해시") \
    X(ChannelFilter, "パラメータ名 / ハッシュ", "Parameter name / hash", "매개변수 이름 / 해시") \
    X(ResetAll, "すべて既定値に戻す", "Reset all to defaults", "모두 기본값으로 되돌리기") \
    X(NoMatches, "一致する項目がありません", "No matches", "일치하는 항목이 없습니다") \
    X(PrevClip, "前のクリップ", "Previous clip", "이전 클립") \
    X(NextClip, "次のクリップ", "Next clip", "다음 클립") \
    X(AnimHeader, "アニメーション (クリップ {})", "Animation (clips {})", "애니메이션 (클립 {})")                         \
    X(PartClips, "選択パーツのクリップ", "Clips of the selected part", "선택한 파츠의 클립")                              \
    X(SkeletonLine, "スケルトン %s (%zu ボーン)   リグ %s", "Skeleton %s (%zu bones)   rig %s",                           \
      "스켈레톤 %s (본 %zu개)   리그 %s")                                                                                  \
    X(SkeletonEstimated, "スケルトンとリグの対応は推定 (共通ボーン・スキンの一致)",                                      \
      "Skeleton-to-rig matching is estimated (shared bones and skin)", "스켈레톤과 리그의 대응은 추정 (공통 본·스킨 일치)")    \
    X(FollowerParts, "連動パーツ %zu", "Linked parts %zu", "연동 파츠 %zu")                                              \
    X(EquipmentCandidate, "装備候補（推定）", "Equipment candidate (estimated)", "장비 후보 (추정)")                      \
    X(AuthoredMotionBank, "このエンティティのアニメーション参照表", "Animations referenced by this entity", "이 엔티티가 참조하는 애니메이션") \
    X(RigCompatibleClips, "リグで再生可能な候補（所属は未確認）", "Rig-compatible clips (ownership unverified)", "리그 호환 후보 (소속 미확인)") \
    X(ShellClipLabel, "[シェル専用]", "[Shell-specific]", "[셸 전용]") \
    X(SharedClipLabel, "[共通]", "[Shared]", "[공통]") \
    X(SharedBodyClip, "共通の人体ボーンで再生（推定）", "Playing shared body bones (estimated)", "공통 신체 본 재생 (추정)") \
    X(SourcePart, "再生元パーツ %s", "Playing from part %s", "재생 원본 파츠 %s")                                         \
    X(NoClips, "対応するクリップが見つかりません", "No matching clips found", "맞는 클립을 찾지 못했습니다")                  \
    X(ClipsSameRig, "ボーン構成が同じリグのクリップを使用", "Using clips from a rig with the same bone layout",            \
      "같은 본 구성의 리그 클립을 사용")                                                                                    \
    X(BindPose, "バインドポーズ", "Bind pose", "바인드 포즈")                                                           \
    X(ClipFrames, "{}  ({} フレーム)", "{}  ({} frames)", "{}  (프레임 {})")                                              \
    X(ClipFramesItem, "{}  {} フレーム##{}", "{}  {} frames##{}", "{}  프레임 {}##{}")                                   \
    X(Additive, "加算  ", "Additive  ", "가산  ")                                                                        \
    X(Seconds, "{:.1f} 秒", "{:.1f} s", "{:.1f}초")                                                                     \
    X(FrameFmt, "フレーム %.1f", "Frame %.1f", "프레임 %.1f")                                                           \
    X(SpeedFmt, "速度 %.2fx", "Speed %.2fx", "속도 %.2fx")                                                              \
    X(LightExposure, "ライト・露出", "Light and exposure", "조명·노출")                                                   \
    X(Exposure, "露出", "Exposure", "노출")                                                                             \
    X(SunAzimuth, "太陽の方位", "Sun azimuth", "태양 방위")                                                             \
    X(SunElevation, "太陽の高度", "Sun elevation", "태양 고도")                                                         \
    X(SunIntensity, "太陽の強さ", "Sun intensity", "태양 세기")                                                         \
    X(SunShadows, "太陽の影", "Sun shadows", "태양 그림자")                                                             \
    X(SunShadowsTip, "nim のシャドウマップ (ゲームのカスケードは未解読のため推定)",                                       \
      "nim's shadow map (the game's cascades are not decoded yet, so estimated)",                                        \
      "nim의 그림자 맵 (게임의 캐스케이드는 아직 해독 전이라 추정)")                                                         \
    X(SSAOTip, "ゲームの hdao と apply_ssao_to_light_buffers (調整値は推定)",                                            \
      "The game's hdao and apply_ssao_to_light_buffers (tuning values estimated)",                                        \
      "게임의 hdao와 apply_ssao_to_light_buffers (조정값은 추정)")                                                          \
    X(SSR, "スクリーンスペース反射", "Screen-space reflections", "스크린 스페이스 반사")                                  \
    X(SSRTip, "ゲームの hi_z_generate_* / ssr_* コンピュートパス (設定値は推定)",                                       \
      "The game's hi_z_generate_* and ssr_* compute passes (settings estimated)",                                        \
      "게임의 hi_z_generate_* / ssr_* 컴퓨트 패스 (설정값은 추정)")                                                         \
    X(AiTip, "AI (推定シミュレーション) とインジケーター", "AI (estimated simulation) and indicators", "AI (추정 시뮬레이션) 와 인디케이터") \
    X(AiEnable, "AI を動かす", "Run the AI", "AI 실행") \
    X(AiEnableTip, "このゲームモードの分隊・目標・巡回をゲームデータから読み、推定の行動で敵を動かす (BlockScript の挙動は未解読)", "Reads this mode's squads, objectives and patrols from game data and moves the enemies with estimated behaviour (BlockScript logic not decoded)", "이 게임 모드의 분대·목표·순찰을 게임 데이터에서 읽어 추정 행동으로 적을 움직임 (BlockScript 동작은 미해독)") \
    X(AiAwareness, "警戒・発見表示", "Alert / detection", "경계·발견 표시") \
    X(AiMarkers, "目標・地点マーカー", "Objective / point markers", "목표·지점 마커") \
    X(AiDebug, "デバッグ表示 (範囲・視界・経路)", "Debug (areas, sight, routes)", "디버그 표시 (범위·시야·경로)") \
    X(AiVisible, "敵にカメラを認識させる", "Let enemies notice the camera", "적이 카메라를 인식") \
    X(AiVisibleTip, "オフ (既定) ではカメラは敵から見えず、近づいても警戒度は上がらない", "Off (default): the camera is invisible to enemies and raises no awareness, however close", "끔 (기본): 카메라는 적에게 보이지 않으며, 가까이 가도 경계도가 오르지 않음") \
    X(AiRange, "表示距離", "Range", "표시 거리") \
    X(AiLoading, "AI を準備中…", "Preparing the AI…", "AI 준비 중…") \
    X(AiHud, "AI 推定: {} 体 (描画 {})  {:.0f} 秒", "AI estimate: {} agents ({} drawn)  {:.0f} s", "AI 추정: {} 체 (표시 {})  {:.0f} 초") \
    X(LogAiPreparing, "AI を準備中: {}", "Preparing the AI: {}", "AI 준비 중: {}") \
    X(LogAiReady, "AI: {} 体、ドローン {}、描画枠 {} (読込 {:.1f} 秒、配置 {:.1f} 秒)", "AI: {} agents, {} drones, {} actor slots (read {:.1f} s, set up {:.1f} s)", "AI: {} 체, 드론 {}, 표시 슬롯 {} (읽기 {:.1f} 초, 배치 {:.1f} 초)") \
    X(ErrAi, "AI を準備できません: {}", "Cannot prepare the AI: {}", "AI 를 준비할 수 없습니다: {}") \
    X(LightShafts, "ライトシャフト", "Light shafts", "라이트 샤프트")                                                    \
    X(LightShaftStrength, "近距離の光芒の強さ", "Near light shaft intensity", "근거리 라이트 샤프트 강도") \
    X(LightShaftsTip, "ライトの volumetrics 技法 → blur → upres (霧密度スライスは生成元未特定のため 0 = 推定)",          \
      "Light volumetrics technique, blur, upres (the fog-density slice has no known source, so 0 = estimated)",           \
      "라이트 volumetrics 기법 → blur → upres (안개 밀도 슬라이스의 원천이 불명확해 0 = 추정)")                              \
    X(PostProcess, "ポストプロセス", "Post-processing", "후처리")                                                       \
    X(PostProcessTip, "ゲームの screen_area_global_lut3d (トーンマップ・グレーディング)。オフで final_combine",             \
      "The game's screen_area_global_lut3d (tone map and grading). Off uses final_combine",                               \
      "게임의 screen_area_global_lut3d (톤맵·그레이딩). 끄면 final_combine을 씁니다")                                         \
    X(AutoExposure, "自動露出", "Auto exposure", "자동 노출")                                                           \
    X(AutoExposureTip, "autoexposure_sample_columns の読み戻し (キー値・適応速度は推定)。露出スライダーは補正",           \
      "Reads back autoexposure_sample_columns (key value and adaptation speed estimated). The exposure slider corrects", \
      "autoexposure_sample_columns를 되읽습니다 (키 값·적응 속도는 추정). 노출 슬라이더는 보정합니다")                         \
    X(Bloom, "ブルーム", "Bloom", "블룸")                                                                                   X(BloomHighlight, "ブルームの光源強調 (推定)", "Bloom highlight weight (estimated)", "블룸 하이라이트 강조 (추정)")     X(BloomHighlightTip, "明るい画素ほど強くにじませる (FX・照明)。0 で線形", "Brighter pixels (FX, lights) spread more; 0 = linear", "밝은 픽셀(FX·조명)일수록 더 번짐. 0 = 선형") \
    X(BloomTip, "ゲームの縮小・ガウスぼかし・加算パス (強さ・カーネルは推定)",                                            \
      "The game's downsample, Gaussian blur and additive passes (strength and kernel estimated)",                        \
      "게임의 축소·가우시안 흐림·가산 패스 (강도·커널은 추정)")                                                              \
    X(TAATip, "ゲームの resolve_taa (ジッター + 動きベクトル + 履歴)。重み・ジッター列は推定。オンの間は FXAA の代わり",       \
      "The game's resolve_taa (jitter, motion vectors, history). Weights and jitter sequence estimated. Replaces FXAA",   \
      "게임의 resolve_taa (지터·모션 벡터·히스토리). 가중치·지터 열은 추정. 켜는 동안 FXAA 대신 씁니다")                     \
    X(SunColor, "太陽の色", "Sun colour", "태양 색")                                                                    \
    X(SkyIntensity, "空の強さ", "Sky intensity", "하늘 세기")                                                           \
    X(SkyZenith, "空 (天頂)", "Sky (zenith)", "하늘 (천정)")                                                            \
    X(SkyHorizon, "空 (地平)", "Sky (horizon)", "하늘 (지평선)")                                                        \
    X(GroundColor, "地面", "Ground", "지면")                                                                            \
    X(BackgroundTop, "背景 (上)", "Background (top)", "배경 (위)")                                                      \
    X(BackgroundBottom, "背景 (下)", "Background (bottom)", "배경 (아래)")                                               \
    X(AtmosphereSection, "大気 (マップの空とフォグ)", "Atmosphere (map sky and fog)", "대기 (맵 하늘과 안개)")            \
    X(Atmosphere, "大気", "Atmosphere", "대기")                                                                         \
    X(AtmosphereTip, "ゲームの sky_lookup_generate_near/far と sky。フォグ値はグローバルチャンネルの写し (推定)",         \
      "The game's sky_lookup_generate_near/far and sky. Fog values mirror the global channels (estimated)",              \
      "게임의 sky_lookup_generate_near/far와 sky. 안개 값은 글로벌 채널의 복사 (추정)")                                      \
    X(FroxelFog, "フロクセル大気", "Froxel fog", "프록셀 대기")                                                         \
    X(InteriorFog, "屋内フォグ", "Interior fog", "실내 안개") \
    X(FogDensityScale, "大気の霧の濃さ", "Atmospheric fog density", "대기 안개 밀도") \
    X(ChromaticAberration, "色収差", "Chromatic aberration", "색수차") \
    X(EmissiveGain, "自己発光の強さ", "Emissive intensity", "자체 발광 강도") \
    X(ParticleStrength, "パーティクルの表示強度", "Particle strength", "파티클 표시 강도") \
    X(OutputGamma, "出力ガンマ", "Output gamma", "출력 감마") \
    X(OutputSharpen, "シャープネス", "Sharpening", "선명도") \
    X(OutputGrain, "画面ノイズ", "Screen grain", "화면 노이즈") \
    X(FroxelFogTip, "ゲームの密度計算・散乱積分CS。格子の解像度・距離・時間履歴は推定",                                    \
      "The game's density and scattering-integration compute shaders. Grid resolution, distance and history estimated",  \
      "게임의 밀도 계산·산란 적분 CS. 격자 해상도·거리·시간 히스토리는 추정")                                               \
    X(MapEnvironment, "マップ環境 ({} チャンネル)", "Map environment ({} channels)", "맵 환경 ({} 채널)")                \
    X(MapEnvironmentTip, "マップの環境エンティティのチャンネル式 (太陽・環境光・霧の時刻カーブ) を時刻で評価",               \
      "Evaluates the map environment entity's channel expressions (sun, ambient and fog time curves) at the current time", \
      "맵 환경 엔티티의 채널 식(태양·환경광·안개 시간 곡선)을 현재 시각으로 평가")                                          \
    X(SkyAmbient, "空の環境光", "Sky ambient light", "하늘 환경광")                                                     \
    X(SkyAmbientTip, "ゲームの空半球マップ (hemisphere_sky_color_generate ほか) と cubemap_apply_sky_copy_ao。日陰の環境光",   \
      "The game's sky hemisphere maps (hemisphere_sky_color_generate etc.) and cubemap_apply_sky_copy_ao. Ambient in shade", \
      "게임의 하늘 반구 맵(hemisphere_sky_color_generate 등)과 cubemap_apply_sky_copy_ao. 그늘의 환경광")                   \
    X(OverheadOcclusion, "頭上の遮蔽", "Overhead occlusion", "머리 위 차폐")                                            \
    X(OverheadOcclusionTip,                                                                                              \
      "cubemap_apply_sky_copy_ao_with_overhead_occlusion: 屋根の下などで空の環境光を弱める。真上からの深度は nim の描画 (推定)、分散マップはゲームの sample_and_blur_variance_map_9x9", \
      "cubemap_apply_sky_copy_ao_with_overhead_occlusion: weakens sky ambient under roofs and similar. Overhead depth comes from nim's own drawing (estimated); variance maps use the game's sample_and_blur_variance_map_9x9", \
      "cubemap_apply_sky_copy_ao_with_overhead_occlusion: 지붕 아래 등에서 하늘 환경광을 약하게 합니다. 머리 위 깊이는 nim 렌더링 (추정), 분산 맵은 게임의 sample_and_blur_variance_map_9x9") \
    X(ProbeFade, "プローブの距離フェード (推定)", "Probe distance fade (estimated)", "프로브 거리 페이드 (추정)")          \
    X(ProbeFadeTip,                                                                                                      \
      "既定では全域の原プローブ照明を描画。オンでカメラ距離によるフェードを使用 (エンジンの判定条件は未解明)。各プローブの空間範囲・色・強度は元データのまま", \
      "By default the original probe lighting is drawn everywhere. On: fades with camera distance (the engine's condition is not known yet). Each probe's extent, colour and strength stay as in the data", \
      "기본은 전체 원본 프로브 조명을 그립니다. 켜면 카메라 거리에 따라 페이드합니다 (엔진 조건은 미확인). 각 프로브의 범위·색·강도는 원본 그대로") \
    X(TimeOwn, "初期時刻を使う (推定)", "Use the default time (estimated)", "기본 시각 사용 (추정)")                    \
    X(TimeOwnTip, "太陽パス成分 +0x08 の値を時刻と仮定。アクティビティ実行時の開始時刻は未特定のため、ゲーム本来の時刻は保証しない", \
      "Assumes the sun path component +0x08 is the time. The start time during activities is not known, so the game's own time is not guaranteed", \
      "태양 경로 성분 +0x08 값을 시각으로 가정합니다. 액티비티 실행 시 시작 시각은 미확인이라 게임 본래 시각을 보장하지 않습니다") \
    X(Time, "時刻", "Time", "시각")                                                                                     \
    X(StaticFilterHint, "  タグ / パッケージ名", "  Tag / package name", "  태그 / 패키지 이름")                         \
    X(NoPath, "未対応パス", "Unsupported path", "미지원 경로")                                                          \
    X(UnsupportedPath, "未対応パス: %s", "Unsupported path: %s", "미지원 경로: %s")                                      \
    X(LodLine, "LOD %u   グループ %zu   半径 %.3f", "LOD %u   groups %zu   radius %.3f", "LOD %u   그룹 %zu   반경 %.3f")  \
    X(NoEntityComponent, "エンティティコンポーネント: なし (パート自身のテクニックのみ)",                                  \
      "Entity component: none (each part's own technique only)", "엔티티 컴포넌트: 없음 (파트 고유 테크닉만)")               \
    X(EntityComponent, "エンティティコンポーネント %s", "Entity component %s", "엔티티 컴포넌트 %s")                     \
    X(MaterialVariant, "マテリアル バリアント", "Material variant", "머티리얼 변형")                                     \
    X(MaterialAnimation, "マテリアルのアニメーション", "Material animation", "머티리얼 애니메이션")                       \
    X(Rewind2, "先頭に戻す##material", "Rewind##material", "처음으로##material")                                         \
    X(TimeFmt, "時刻 %.2f 秒", "Time %.2f s", "시각 %.2f초")                                                            \
    X(ShadingLine, "シェーディング: %s (ゲームの global_lighting_and_shading)",                                          \
      "Shading: %s (the game's global_lighting_and_shading)", "셰이딩: %s (게임의 global_lighting_and_shading)")         \
    X(PartHeader, "パート {}  ({} インデックス)", "Part {}  ({} indices)", "파트 {}  (인덱스 {})")                       \
    X(Technique, "テクニック  %s", "Technique  %s", "테크닉  %s")                                                       \
    X(CbufferLine, "cbuffer %zu レジスタ   TFX %zu 命令", "cbuffer %zu registers   TFX %zu instructions",               \
      "cbuffer 레지스터 %zu개   TFX 명령 %zu개")                                                                          \
    X(StubbedOp, "未実装 TFX op: %s", "Unimplemented TFX op: %s", "미구현 TFX op: %s")                                  \
    X(EngineSide, "エンジン側 (+0x%X)", "Engine side (+0x%X)", "엔진 측 (+0x%X)")                                       \
    X(TextFilterHint, "  検索 (英語・選択中の言語・ハッシュ)", "  Search (English, chosen language, hash)",               \
      "  검색 (영어·선택한 언어·해시)")                                                                                    \
    X(TextCount, "%zu / %zu 件  (右クリックでコピー)", "%zu / %zu entries  (right-click to copy)",                    \
      "%zu / %zu개  (오른쪽 클릭으로 복사)")                                                                              \
    X(CopyEnglish, "英語をコピー", "Copy English", "영어 복사")                                                         \
    X(CopyLanguage, "{} をコピー", "Copy {}", "{} 복사")                                                                 \
    X(CopyHash, "ハッシュをコピー", "Copy hash", "해시 복사")                                                           \
    X(CopyDisplayed, "表示言語をコピー", "Copy displayed text", "표시 언어 복사")                                        \
    X(DialogueFilterHint, "  話者・台詞で検索", "  Search speaker or line", "  화자·대사 검색")                          \
    X(ShowEnglish, "英語を併記", "Show English too", "영어 병기")                                                       \
    X(NoVoicePackages, "音声パッケージなし", "No voice packages", "음성 패키지 없음")                                     \
    X(VoiceTip, "インストール済みの音声言語パッケージ (Steam の言語設定で追加できます)",                                  \
      "Installed voice-language packages (more can be added in Steam's language settings)",                               \
      "설치된 음성 언어 패키지 (Steam 언어 설정에서 추가할 수 있습니다)")                                                    \
    X(ExportTip, "表示中の台詞を WAV で話者ごとのフォルダに書き出し (index.csv に全言語のテキスト)",                       \
      "Writes the lines shown as WAV into one folder per speaker (index.csv has the text in every language)",            \
      "표시 중인 대사를 WAV로 화자별 폴더에 내보냅니다 (index.csv에 모든 언어의 텍스트)")                                     \
    X(AudioError, "音声エラー: %s", "Audio error: %s", "음성 오류: %s")                                                  \
    X(ByTable, "会話テーブル", "Conversation table", "대화 테이블")                                                     \
    X(BySpeaker, "話者", "Speaker", "화자")                                                                             \
    X(AllLines, "すべて  ({})", "All  ({})", "전체  ({})")                                                                \
    X(RowCount, "%zu / %zu 行", "%zu / %zu lines", "%zu / %zu줄")                                                       \
    X(ExportRunning, "{} 本を {} 音声で書き出し中…", "Exporting {} files in {} voice…", "{}개를 {} 음성으로 내보내는 중…") \
    X(ExportDone, "{}{} / {} 本を書き出しました ({} 音声, 未収録 {}, 失敗 {}): {}",                                      \
      "{}{} / {} files exported ({} voice, not recorded {}, failed {}): {}",                                              \
      "{}{} / {}개 내보냄 ({} 음성, 미수록 {}, 실패 {}): {}")                                                               \
    X(ExportAborted, "中止: ", "Stopped: ", "중지: ")                                                                    \
    X(LineInfo, "行 %08X  テーブル %s  音声イベント %s", "Line %08X  table %s  audio event %s",                          \
      "줄 %08X  테이블 %s  음성 이벤트 %s")                                                                               \
    X(NoAudio, "音声なし", "No audio", "음성 없음")                                                                      \
    X(SaveWavTip, "選択中の音声言語で WAV に保存", "Save as WAV in the chosen voice language",                          \
      "선택한 음성 언어로 WAV 저장")                                                                                       \
    X(ErrWav, "WAV を保存できません: {}", "Cannot save WAV: {}", "WAV를 저장할 수 없습니다: {}")                        \
    X(NoRecording, "この言語の音声は収録されていません", "No recording exists for this language",                      \
      "이 언어의 음성은 수록되지 않았습니다")                                                                              \
    X(ErrPlayAudio, "音声を再生できません: {}", "Cannot play audio: {}", "음성을 재생할 수 없습니다: {}")                 \
    X(LogSaved, "保存しました: {}", "Saved: {}", "저장했습니다: {}")                                                    \
    X(ErrSave, "保存に失敗: {}", "Save failed: {}", "저장 실패: {}")                                                    \
    X(MovieCount, "%zu 本 (多言語音声 %zu)", "%zu movies (multilingual audio %zu)", "%zu개 (다국어 음성 %zu)")            \
    X(CodexFilterHint, "タイトル・本文で検索", "Search titles and text", "제목·본문 검색") \
    X(CodexAudioOnly, "音声ログのみ", "Audio logs only", "음성 로그만") \
    X(CodexCount, "{} コレクション / {} 記録 / 音声 {}", "{} collections / {} records / {} with audio", "{} 컬렉션 / {} 기록 / 음성 {}") \
    X(CodexNone, "該当する記録はありません。", "No records match.", "해당하는 기록이 없습니다.") \
    X(CodexPick, "左の一覧から記録を選んでください。", "Choose a record from the list on the left.", "왼쪽 목록에서 기록을 고르세요.") \
    X(CodexUnlock, "解除条件", "unlock", "해제 조건") \
    X(CodexPlayLog, "音声ログ", "audio log", "음성 로그") \
    X(CodexAssetNoAudio, "録音アセット {} はありますが、対応する台詞が見つかりません", "Recording asset {} has no matching dialogue line", "녹음 에셋 {} 에 맞는 대사가 없습니다") \
    X(CodexNoBody, "(本文なし)", "(no text)", "(본문 없음)") \
    X(CodexNoIllustration, "イラスト: コデックスのデータにテクスチャ参照が見つかっていません (未解明)", "Illustration: the codex data references no texture that nim has found (unresolved)", "일러스트: 코덱스 데이터에서 텍스처 참조를 찾지 못했습니다 (미해명)") \
    X(MovieFilterHint, "  ファイル名で検索", "  Search file name", "  파일 이름 검색")                                 \
    X(AudioTrack, "音声トラック", "Audio track", "음성 트랙")                                                           \
    X(TrackTip, "言語トラックの割り当てはゲームデータに無いため推定です\n(チャンネル番号 = 文字列の言語スロット + 1。繁体字中国語の ch9 は欠番)", \
      "The language-track mapping is not in the game data, so it is estimated\n(channel = text language slot + 1; ch9 for Traditional Chinese is unused)", \
      "언어 트랙 대응은 게임 데이터에 없어 추정입니다\n(채널 = 텍스트 언어 슬롯 + 1. 번체 중국어 ch9은 비어 있음)")         \
    X(TrackEstimated, "{} (推定)", "{} (estimated)", "{} (추정)")                                                       \
    X(TrackDefault, "既定", "Default", "기본")                                                                          \
    X(VideoOnlyUnsupported, "この音声 (%s) は未対応のため映像のみ", "This audio (%s) is unsupported, so video only",       \
      "이 음성(%s)은 미지원이라 영상만 재생합니다")                                                                         \
    X(AudioOutputError, "音声出力を開けません", "Cannot open the audio output", "음성 출력을 열 수 없습니다")              \
    X(NoSubtitlesYet, "字幕は再生に合わせて読み込まれます (このムービーにはまだ字幕がありません)",                         \
      "Subtitles load as the movie plays (this movie has no subtitles yet)",                                              \
      "자막은 재생에 맞춰 불러옵니다 (이 영상에는 아직 자막이 없습니다)")                                                    \
    X(MovieFrameError, "ムービーのフレームを転送できません: {}", "Cannot transfer movie frame: {}",                    \
      "영상 프레임을 전송할 수 없습니다: {}")                                                                              \
    X(ErrOpenPackage, "パッケージを開けません: {}", "Cannot open package: {}", "패키지를 열 수 없습니다: {}")              \
    X(EntryFilterHint, "  種別 (例 8, 32/1) またはクラス", "  Type (e.g. 8, 32/1) or class",                         \
      "  종류 (예: 8, 32/1) 또는 클래스")                                                                                   \
    X(StaticMeshTag, "  (static mesh)", "  (static mesh)", "  (static mesh)")                                          \
    X(DynamicModelTag, "  (dynamic model)", "  (dynamic model)", "  (dynamic model)")                                  \
    X(LogEmpty, "まだ記録はありません", "No messages yet", "아직 기록이 없습니다")                                         \
    X(ErrPackagesMissing, "packages フォルダが見つからないか空です", "The packages folder is missing or empty",         \
      "packages 폴더가 없거나 비어 있습니다")                                                                              \
    X(LogInstallOpened, "インストールを開きました: {} ({} パッケージ)", "Opened install: {} ({} packages)",              \
      "설치 폴더를 열었습니다: {} ({}개 패키지)")                                                                           \
    X(ErrOpenInstall, "インストールを開けません: {}", "Cannot open the install: {}", "설치 폴더를 열 수 없습니다: {}")       \
    X(LogLighting, "ゲームのライティング: global_lighting_and_shading ({})",                                             \
      "Game lighting: global_lighting_and_shading ({})", "게임 조명: global_lighting_and_shading ({})")                   \
    X(ErrLighting, "ゲームのライティングを使えません (代替描画は無効): {}",                                               \
      "Game lighting is unavailable (no substitute rendering): {}", "게임 조명을 쓸 수 없습니다 (대체 렌더링 없음): {}")       \
    X(ErrEntityLight, "エンティティの環境光: {}", "Entity ambient light: {}", "엔티티 환경광: {}")                        \
    X(LogModelLoaded, "{} を読込 ({} グループ, 準備 {:.0f} ms + GPU {:.0f} ms){}",                                       \
      "Loaded {} ({} groups, prepare {:.0f} ms + GPU {:.0f} ms){}", "{} 불러옴 (그룹 {}개, 준비 {:.0f} ms + GPU {:.0f} ms){}") \
    X(LogStubbed, ", 未実装 TFX op {} 種", ", {} unimplemented TFX ops", ", 미구현 TFX op {}종")                        \
    X(ErrDraw, "{} を描画できません: {}", "Cannot draw {}: {}", "{}을(를) 그릴 수 없습니다: {}")                         \
    X(ErrRead, "{} を読めません: {}", "Cannot read {}: {}", "{}을(를) 읽을 수 없습니다: {}")                              \
    X(ErrRenderStopped, "描画停止 (代替なし): {}", "Rendering stopped (no substitute): {}",                               \
      "렌더링 중지 (대체 없음): {}")                                                                                       \
    X(RenderStopped, "描画停止: ゲーム由来の入力が不足しています (代替描画なし)",                                          \
      "Rendering stopped: game-derived inputs are missing (no substitute rendering)",                                    \
      "렌더링 중지: 게임 유래 입력이 부족합니다 (대체 렌더링 없음)")                                                          \
    X(ErrRender, "描画エラー: {}", "Render error: {}", "렌더링 오류: {}")                                                 \
    X(ErrDeviceRemoved, "GPU がエラーで停止したため終了します (device removed)。\nNIM_D3D_DEBUG=1 を設定して起動すると nim_d3d.log に詳細が出ます。\n\n{}", \
      "Quitting because the GPU stopped with an error (device removed).\nStart with NIM_D3D_DEBUG=1 to write details to nim_d3d.log.\n\n{}", \
      "GPU가 오류로 멈춰 종료합니다 (device removed).\nNIM_D3D_DEBUG=1로 시작하면 nim_d3d.log에 자세히 남습니다.\n\n{}")  \
    X(ErrD3D12, "D3D12 の初期化に失敗しました:\n{}", "Failed to initialise D3D12:\n{}", "D3D12 초기화에 실패했습니다:\n{}")  \
    X(ErrCreated, "{} を作れません", "Cannot create {}", "{}을(를) 만들 수 없습니다")                                   \
    X(ErrLoad, "読込に失敗: {}", "Load failed: {}", "불러오기 실패: {}")                                                \
    X(ErrPaintNoCapture, "保存する画像がありません", "There is no image to save", "저장할 이미지가 없습니다")              \
    X(LogIdle, "準備完了", "Ready", "준비 완료")                                                                        \
    X(ErrDialogueLoad, "セリフの読込に失敗", "Failed to load dialogue", "대사를 불러오지 못했습니다")                    \
    X(ErrWriteFailed, "書き込み失敗", "Write failed", "쓰기 실패")                                                      \
    X(ErrNoRecordingLog, "音声を再生できません", "Cannot play audio", "음성을 재생할 수 없습니다")                        \
    X(ErrWriteWav, "書き込めません", "Cannot write the file", "파일을 쓸 수 없습니다")                                     \
    X(FrameAll, "全体を表示 (F)", "Frame all (F)", "전체 보기 (F)")                                                      \
    X(SavePng, "PNG で保存 (Ctrl+S)", "Save PNG (Ctrl+S)", "PNG로 저장 (Ctrl+S)")                                         \
    X(NoSpeaker, "(話者なし)", "(no speaker)", "(화자 없음)")                                                              \
    X(UnknownSpeaker, "(不明 {:08X})", "(unknown {:08X})", "(알 수 없음 {:08X})")                                          \
    X(Others, " 他", " others", " 외")                                                                                    \
    X(LangEnglish, "英語", "English", "영어")                                                                           \
    X(LangJapanese, "日本語", "Japanese", "일본어")                                                                     \
    X(LangGerman, "ドイツ語", "German", "독일어")                                                                       \
    X(LangFrench, "フランス語", "French", "프랑스어")                                                                   \
    X(LangSpanish, "スペイン語", "Spanish", "스페인어")                                                                 \
    X(LangSpanishLatam, "スペイン語 (中南米)", "Spanish (Latin America)", "스페인어 (중남미)")                            \
    X(LangItalian, "イタリア語", "Italian", "이탈리아어")                                                               \
    X(LangKorean, "韓国語", "Korean", "한국어")                                                                         \
    X(LangTraditionalChinese, "繁体字中国語", "Traditional Chinese", "번체 중국어")                                       \
    X(LangSimplifiedChinese, "簡体字中国語", "Simplified Chinese", "간체 중국어")                                         \
    X(LangPortugueseBrazil, "ポルトガル語 (ブラジル)", "Portuguese (Brazil)", "포르투갈어 (브라질)")                       \
    X(LangPolish, "ポーランド語", "Polish", "폴란드어")                                                                 \
    X(LangRussian, "ロシア語", "Russian", "러시아어")

enum class S : uint16_t {
#define NIM_UI_ENUM(key, ja, en, ko) key,
    NIM_UI_STRINGS(NIM_UI_ENUM)
#undef NIM_UI_ENUM
        Count
};

// The interface language. Japanese until a settings file says otherwise.
Lang lang();
void set_lang(Lang l);

// The text of a key in the interface language, or in a given language (for tests and tables).
const char* tr(S s);
const char* tr_in(S s, Lang l);

// std::format-style text: the key's text with its {} placeholders filled in.
template <class... A>
std::string trf(S s, A&&... a) {
    return std::vformat(tr(s), std::make_format_args(a...));
}

// Native names of the interface languages, for the language selector.
const char* native_name(Lang l);
// Short codes stored in the settings file: "ja", "en", "ko".
const char* code(Lang l);
bool parse_lang(std::string_view code, Lang& out);

// The 13 content text languages in TextLanguage order (game content, not interface text).
const char* content_language_name(size_t index);

} // namespace nim::ui
