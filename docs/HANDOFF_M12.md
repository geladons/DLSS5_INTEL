# HANDOFF_M12.md - промпт для следующего агента (2026-09-23)
# Скопируй всё ниже линии как промпт новой сессии. Держи его под рукой.
# ==================================================================

## ПРОМПТ (копировать отсюда)

Принимаешь разработку проекта DLSS5_INTEL (C:\Users\AI\Desktop\DLSS5_INTEL).
Цель проекта: настоящий DLSS 5 (проприетарные веса dlssnr, 71-блочный U-Net)
живьём на Intel Arc Pro B50 — обработка кадров игры на презенте через
Vulkan-слой и TCP-даемон. Хозяин играет через Sunshine/Moonlight.

ПЕРВЫМ ДЕЛОМ прочитай AGENTS.md и DEV_STATE.md в корне проекта — там правила,
грабли хоста и текущее состояние. Соблюдай AGENTS.md буквально (ASCII-only в
.cmd, детач длинных процессов через start в .cmd, выключать за собой
процессы, Sunshine не трогать, ООП-правила кода, коммиты мелкие).

Что уже работает (не переизобретать, не ломать):
- dlss5\m11-layer — Windows Vulkan implicit layer (порт reference
  nr_layer.c). Перехватывает vkQueuePresentKHR любого Vulkan-приложения,
  шлёт кадр по TCP 127.0.0.1:47990 даемону, пишет ответ обратно в свопчейн.
  Зарегистрирован в HKCU ImplicitLayers, always-on (выкл: DISABLE_NR_LAYER=1).
  Сборка: cmd //c dlss5\m11-layer\build.cmd.
- dlss5\m11d — даемон с НАСТОЯЩЕЙ моделью. Сборка: cmd //c
  dlss5\m11d\build.cmd (VS generator сломан на хосте -> NMake fallback;
  рабочий exe: dlss5\m11d\build-nmake\m11d.exe, шейдеры .spv лежат рядом,
  запускать с CWD=build-nmake). Запуск демо: dlss5\m11d\_demo.cmd (m11d +
  vkcube через слой). Валидация против torch-голденов:
  m11d.exe --selftest C:\Users\AI\Desktop\DLSS5_INTEL\work\_m9b_cmp\native_crop.bmp
  из CWD=build-nmake, затем сравнить build-nmake\out\live_featV.bin /
  live_head.bin с work\_m9b_cmp\golden_features.bin / golden_head.bin
  (features: 15/16 каналов bit-exact; head meandiff ~0.0083 = норма).
- dlss5\chain — ООП-модуль модели (VkContext/WeightsStore/ChainArena/
  ChainRecorder/ChainEngine). m8b-live пока НЕ мигрирован на него (стоит
  отдельная задача; миграцию валидировать _go10.cmd/_go9.cmd).
- Производительность чейна: ~135 мс на 500x500, ~930 мс на 1344x1088
  (GEMM-лимитировано, см. DEV_STATE.md M9b). На полноценном игровом окне
  это slideshow — это ОЖИДАЕМО, режим слоя NR_LAYER_LIVE N обрабатывает
  каждый N-й презент, между ними ре-блит последнего (см. README слоя /
  nr_layer_win.c шапку). Для живой игры предложи хозяину окно поменьше
  или LIVE=4..8.

ТЕКУЩАЯ ЗАДАЧА (M12): живая игра. Хозяин запустил GTA 5 в ОКОННОМ режиме.
ВАЖНО: у него GTA5_Enhanced.exe (D:\Grand Theft Auto V Enhanced\) — это
DX12 + BattleEye (процесс GTA5_Enhanced_BE.exe). Наш слой — только Vulkan.
Два пути, обсуди с хозяином до действий:
  A. РЕКОМЕНДУЕМЫЙ: GTA5 Legacy (DX11, у владельцев Enhanced бесплатна в
     Rockstar/Steam) + DXVK x64 (d3d11.dll + dxgi.dll из релиза DXVK на
     GitHub в папку игры рядом с GTA5.exe) -> игра идёт через Vulkan ->
     слой перехватывает. Для DX9-игр понадобилась бы 32-битная сборка
     слоя (манифест library_arch 32, отдельная DLL), для GTA5 Legacy x64
     НЕ нужна.
  B. Enhanced + vkd3d-proton (d3d12.dll рядом с exe) — DX12->Vulkan.
     РИСКИ: BattleEye может блокировать посторонние DLL в папке игры;
     только сюжетный режим, никакого GTA Online.
Порядок действий (путь A): хозяин ставит Legacy -> копируешь DXVK x64
dll -> запускаешь m11d (консоль видна) -> хозяин запускает игру в окне ->
смотришь логи m11d (frame WxH ...) и %TEMP%\nr_layer_win.log. Эффект
модели тонкий (откалибровано: mean|d| ~2/255) — для наглядности есть
--gain 2 у m11d (вендорский intensity до 2). Первая обработка кадра
блокирует ~6 с (загрузка весов) — таймаут слоя 60 с, всё ок.
Проверка зрительная у хозяина в стриме (локальные скриншоты Vulkan-окон
на этом хосте ЧЁРНЫЕ — GDI и DDA слепы к flip-окнам, не паникуй).
Не путай: окно игры должно быть НЕ fullscreen-exclusive (borderless/windowed).

Когда заработает — спроси хозяина про дальше: производительность
(M10: тюнинг GEMM-шейдеров в dlss5\m8b-live\shaders\m8\gemm.comp, потолок
~30 TF/s vs текущие 0.6-2.9) или миграция m8b-live на dlss5\chain.

## /ПРОМПТ (конец)
