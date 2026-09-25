# ============================================================================
# m13.i18n - UI strings. English is the primary language, Russian optional
# (Settings -> Language; stored in config.json as "language": en|ru).
# Usage: from .i18n import tr ; tr("key", arg) - plain %-formatting.
# ============================================================================

_S = {
    # ---- main window ------------------------------------------------------
    "app_title": {"en": "DLSS 5 Manager", "ru": "DLSS 5 Менеджер"},
    "app_subtitle": {
        "en": "the real 71-block neural chain, live on your GPU",
        "ru": "реальная 71-блочная нейросеть, живая, на твоей видеокарте"},
    "overlay_button": {"en": "In-game overlay (%s)", "ru": "Игровой оверлей (%s)"},
    "daemon_start": {"en": "Start daemon", "ru": "Запустить демон"},
    "daemon_stop": {"en": "Stop", "ru": "Стоп"},
    "gain": {"en": "gain", "ru": "сила"},
    "autosetup": {"en": "Auto-setup:", "ru": "Автонастройка:"},
    "step_weights": {"en": "Weights", "ru": "Веса"},
    "step_layers": {"en": "Vulkan layers", "ru": "Слои Vulkan"},
    "step_daemon": {"en": "Daemon", "ru": "Демон"},
    "step_game": {"en": "Game ready", "ru": "Игра готова"},
    "daemon_up": {"en": "daemon: up (pid %s)", "ru": "демон: работает (pid %s)"},
    "daemon_up_stats": {"en": " - gain %.2f, %d frames",
                        "ru": " - сила %.2f, %d кадров"},
    "daemon_busy": {"en": " [busy]", "ru": " [занят]"},
    "daemon_down": {"en": "daemon: down", "ru": "демон: остановлен"},
    "layers_state": {"en": "layers: x64 %s / x86 %s", "ru": "слои: x64 %s / x86 %s"},
    "hint_no_weights": {
        "en": "Weights not found - Settings tab, point at dlssnr-logical.safetensors",
        "ru": "Не найдены веса - вкладка «Настройки», укажи dlssnr-logical.safetensors"},
    "hint_working": {"en": "Setting up automatically, a few seconds...",
                     "ru": "Настраиваю сам, несколько секунд..."},
    "hint_pick_game": {
        "en": "Pick a game card in the Games tab and press 'Enable DLSS 5'",
        "ru": "Выбери игру карточкой на вкладке «Игры» и нажми «Включить DLSS 5»"},
    "hint_ready": {
        "en": "All set! Press 'Play'; in game %s opens the control panel.",
        "ru": "Всё готово! Жми «Играть»; в игре %s открывает панель управления."},
    "tab_games": {"en": "  Games  ", "ru": "  Игры  "},
    "tab_screen": {"en": "  Screen  ", "ru": "  Экран  "},
    "tab_settings": {"en": "  Settings  ", "ru": "  Настройки  "},
    "egg_title": {"en": "DLSS 5 - the Secret Fifth",
                  "ru": "DLSS 5 «Пятёрочка»"},
    "egg_log": {
        "en": "EASTER EGG: the network was trained on 10,000 hours of GTA IV "
              "and one very patient owner.",
        "ru": "ПАСХАЛКА: нейросеть обучена на 10 000 часах GTA IV и одном "
              "очень терпеливом владельце."},
    # ---- screen tab -------------------------------------------------------
    "screen_note": {
        "en": "Fullscreen desktop overlay (watch it through your streaming "
              "client). NOTE: the daemon and the screen overlay cannot run "
              "together - each needs ~12 GB of VRAM, few GPUs can hold both. "
              "Starting one stops the other automatically.",
        "ru": "Полноэкранный оверлей рабочего стола (смотри через трансляцию "
              "Moonlight). ВАЖНО: демон и оверлей экрана не живут вместе - "
              "каждому нужно ~12 ГБ видеопамяти. При запуске одного второй "
              "останавливается автоматически."},
    "screen_start": {"en": "Start fullscreen overlay",
                     "ru": "Запустить оверлей экрана"},
    "screen_stop": {"en": "Stop", "ru": "Стоп"},
    "screen_hotkeys": {"en": "hotkeys: CTRL+ALT+X hide/show, CTRL+ALT+Q quit",
                       "ru": "горячие клавиши: CTRL+ALT+X скрыть/показать, "
                             "CTRL+ALT+Q выход"},
    "screen_window_mode": {"en": "Window mode - title contains:",
                           "ru": "Режим окна - заголовок содержит:"},
    "screen_window_start": {"en": "Start on window", "ru": "Запустить на окне"},
    "screen_state_up": {"en": "overlay: UP (pid %s, warming up - first frames "
                              "are slow)", "ru": "оверлей: РАБОТАЕТ (pid %s, "
                              "греется - первые кадры долгие)"},
    "screen_state_off": {"en": "overlay: off", "ru": "оверлей: выкл"},
    "screen_warmup": {
        "en": "Warm-up at 1440p: ~30 s weights upload, up to a minute for the "
              "first frame - it is loading, not dead. For a quick demo use "
              "window mode above.",
        "ru": "Прогрев 1440p: загрузка весов ~30 с, первый кадр до минуты - "
              "это загрузка, а не зависание. Для быстрого демо используй "
              "режим окна выше."},
    # ---- settings tab -----------------------------------------------------
    "weights_label": {"en": "weights (.safetensors):", "ru": "веса (.safetensors):"},
    "browse": {"en": "Browse...", "ru": "Обзор..."},
    "save": {"en": "Save", "ru": "Сохранить"},
    "layers_register": {"en": "Register Vulkan layers (HKCU)",
                        "ru": "Зарегистрировать слои Vulkan (HKCU)"},
    "layers_unregister": {"en": "Unregister", "ru": "Снять регистрацию"},
    "layers_unknown": {"en": "layers: ?", "ru": "слои: ?"},
    "opt_freeze": {
        "en": "Freeze the frame while the overlay is open (photo mode with "
              "live preview)",
        "ru": "Замораживать кадр, пока оверлей открыт (фото-режим с живым "
              "превью)"},
    "opt_autopause": {"en": "Pause processing when the overlay opens",
                      "ru": "Ставить обработку на паузу при открытии оверлея"},
    "opt_focus": {
        "en": "Give the overlay keyboard/mouse focus (releases the game "
              "cursor; exclusive-fullscreen games may minimize)",
        "ru": "Отдавать оверлею фокус мыши (освобождает курсор игры; "
              "эксклюзивный полноэкранный режим может свернуться)"},
    "language_label": {"en": "Language / Язык:", "ru": "Language / Язык:"},
    "language_note": {"en": "takes effect after restart",
                      "ru": "применится после перезапуска"},
    "hotkey_note": {"en": "Overlay hotkey: %s (change in config.json)",
                    "ru": "Горячая клавиша оверлея: %s (меняется в config.json)"},
    "config_note": {"en": "Config: %s", "ru": "Конфиг: %s"},
    # ---- games tab --------------------------------------------------------
    "rescan": {"en": "Rescan", "ru": "Пересканировать"},
    "add_manual": {"en": "Add exe manually...", "ru": "Добавить exe вручную..."},
    "scanning": {"en": "scanning drives (up to ~20 s)...",
                 "ru": "сканирую диски (до ~20 с)..."},
    "scan_failed": {"en": "scan failed: %s", "ru": "ошибка сканирования: %s"},
    "games_found": {"en": "games found: %d", "ru": "найдено игр: %d"},
    "mode_label": {"en": "mode:", "ru": "режим:"},
    "mode_auto": {"en": "auto", "ru": "авто"},
    "enable": {"en": "Enable DLSS 5", "ru": "Включить DLSS 5"},
    "play": {"en": "Play", "ru": "Играть"},
    "pause_resume": {"en": "Pause / Resume", "ru": "Пауза / продолжить"},
    "disable": {"en": "Disable", "ru": "Отключить"},
    "remove": {"en": "Remove", "ru": "Убрать"},
    "anticheat_warn": {
        "en": "⚠ Anti-cheat: do not enable in online/protected titles (PUBG, "
              "CS2, GTA Online) - DLL injection can be read as a cheat.",
        "ru": "⚠ Античит: не включай в онлайн-играх (PUBG, CS2, GTA Online) - "
              "внедрение DLL могут посчитать читом."},
    "pick_game": {"en": "pick a game card first",
                  "ru": "сначала выбери игру из карточек"},
    "pick_exe": {"en": "game exe", "ru": "exe игры"},
    "state_not_added": {"en": "not added", "ru": "не добавлена"},
    "state_added": {"en": "added", "ru": "добавлена"},
    "state_enabled": {"en": "DLSS 5 enabled", "ru": "DLSS 5 включён"},
    "state_running": {"en": "RUNNING", "ru": "ЗАПУЩЕНА"},
    "state_paused": {"en": " (paused)", "ru": " (пауза)"},
    "state_foreign": {"en": "foreign DLLs! check the folder",
                      "ru": "чужие DLL! проверь папку"},
    "state_anticheat": {"en": "⚠ anti-cheat risk", "ru": "⚠ античит-риск"},
    "via": {"en": "via %s", "ru": "через %s"},
    "saved_mark": {"en": "saved", "ru": "сохранена"},
    # ---- overlay ----------------------------------------------------------
    "ov_pause": {"en": "PAUSE PROCESSING", "ru": "ПАУЗА ОБРАБОТКИ"},
    "ov_resume": {"en": "RESUME PROCESSING", "ru": "ВОЗОБНОВИТЬ ОБРАБОТКУ"},
    "ov_target": {"en": "target: %s", "ru": "цель: %s"},
    "ov_target_none": {"en": "nothing running", "ru": "ничего не запущено"},
    "ov_game_dx9": {"en": "DX9 game", "ru": "игра DX9"},
    "ov_game_dx11": {"en": "DX11 game", "ru": "игра DX11"},
    "ov_game_dx12": {"en": "DX12 game", "ru": "игра DX12"},
    "ov_game_vulkan": {"en": "Vulkan game", "ru": "игра Vulkan"},
    "ov_screen": {"en": "Screen mode", "ru": "режим экрана"},
    "ov_freeze_on": {
        "en": "❄ Frame frozen. Turn the knob - the ORIGINAL frame is "
              "reprocessed with the new gain.",
        "ru": "❄ Кадр заморожен. Крути ползунок - переобработаю исходный "
              "кадр с новой силой."},
    "ov_freeze_suspend": {
        "en": "❄ Game suspended (started outside the manager - no live "
              "preview).",
        "ru": "❄ Игра приостановлена (запущена не из менеджера - живого "
              "превью нет)."},
    "ov_gain": {"en": "Intensity", "ru": "Сила эффекта"},
    "ov_reset": {"en": "Reset 1.0", "ru": "Сброс 1.0"},
    "ov_autopause": {"en": "auto-pause on open", "ru": "пауза при открытии"},
    "ov_freeze_opt": {"en": "freeze the frame while this panel is open",
                      "ru": "замораживать кадр, пока панель открыта"},
    "ov_daemon_up": {"en": "daemon up (pid %s)", "ru": "демон работает (pid %s)"},
    "ov_daemon_stats": {"en": " - gain %.2f, %d frames",
                        "ru": " - сила %.2f, кадров %d"},
    "ov_screen_up": {"en": "screen overlay up (pid %s)",
                     "ru": "оверлей экрана работает (pid %s)"},
    "ov_daemon_down": {"en": "daemon down", "ru": "демон остановлен"},
    "ov_hide_hint": {"en": "%s - hide the panel", "ru": "%s - скрыть панель"},
    # ---- splash -----------------------------------------------------------
    "splash_subtitle": {"en": "neural upscaling, live on your GPU",
                        "ru": "нейросетевой апскейл на твоей видеокарте"},
}

SPLASH_LINES = {
    "en": [
        "Waking the neural network...",
        "Politely asking the GPU...",
        "Loading all 71 DLSS 5 blocks...",
        "Fine-tuning on cat pictures...",
        "Ironing the pixels...",
        "Asking Vulkan not to crash...",
        "Warming up cooperative matrices...",
        "Almost there. Secret: the net fears GTA IV too.",
    ],
    "ru": [
        "Будим нейросеть...",
        "Уговариваем видеокарту...",
        "Загружаем 71 блок DLSS 5...",
        "Дообучаем на котиках...",
        "Разглаживаем пиксели...",
        "Просим Vulkan не падать...",
        "Колдуем кооперативные матрицы...",
        "Почти готово. Секрет: нейросеть тоже боится GTA IV.",
    ],
}

_LANG = "en"


def set_language(lang):
    global _LANG
    _LANG = "ru" if str(lang).lower().startswith("ru") else "en"


def get_language():
    return _LANG


def tr(key, *args):
    text = _S.get(key, {}).get(_LANG) or _S.get(key, {}).get("en") or key
    return text % args if args else text


def splash_lines():
    return SPLASH_LINES.get(_LANG, SPLASH_LINES["en"])
