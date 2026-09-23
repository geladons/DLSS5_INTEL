# HANDOFF_M12.md - промпт для следующего агента (2026-09-23, v2)
# ==================================================================

## ПРОМПТ (копировать отсюда)

Принимаешь разработку проекта DLSS5_INTEL (C:\Users\AI\Desktop\DLSS5_INTEL).
Цель: настоящий DLSS 5 (проприетарные веса dlssnr, 71-блочный U-Net) живьём
на Intel Arc Pro B50 — обработка кадров игры на презенте. Хозяин играет через
Sunshine/Moonlight (стрим), Sunshine НЕ трогать никогда.

ПЕРВЫМ ДЕЛОМ прочитай AGENTS.md и DEV_STATE.md в корне проекта — правила,
грабли хоста, текущее состояние. Соблюдай AGENTS.md буквально: ASCII-only в
.cmd и комментариях кода; длинные процессы запускать детачнуто через .cmd с
start внутри и смотреть логи файлов; после тестов убивать свои процессы
("выключай за собой"); ООП-правила кода (новая подсистема = свой модуль,
потолок ~600 строк/файл); мелкие коммиты; веса и reference\ в git не
коммитить; git = "C:\Program Files\Git\cmd\git.exe".

ЧТО УЖЕ РАБОТАЕТ (не ломать, не переизобретать):
- dlss5\m11-layer — Windows Vulkan implicit layer (порт reference
  nr_layer.c). Перехватывает vkQueuePresentKHR ЛЮБОГО Vulkan-приложения,
  TCP-раундтрип кадра к даемону 127.0.0.1:47990, ответ обратно в свопчейн.
  Зарегистрирован в HKCU ImplicitLayers, always-on (выкл: DISABLE_NR_LAYER=1).
  Сборка: cmd //c dlss5\m11-layer\build.cmd (VS generator сломан -> NMake
  fallback внутри build.cmd, это норма).
- dlss5\m11d — TCP-даемон с НАСТОЯЩЕЙ моделью. Рабочий exe:
  dlss5\m11d\build-nmake\m11d.exe (CWD при запуске = build-nmake, шейдеры .spv
  там же). Сборка: cmd //c dlss5\m11d\build.cmd. Протокол: 16-байтный хедер
  {magic=0x304E524E, w, h, VkFormat} + payload w*h*4 BGRA -> ответ w*h*4.
  Входная альфа сохраняется. --gain F = вендорский intensity (до 2; эффект
  модели тонкий по умолчанию, mean|d| ~2/255 - ЭТО НОРМА, откалибровано
  против torch-референса). Демо: dlss5\m11d\_demo.cmd (m11d + vkcube).
- dlss5\chain — ООП-модуль модели (VkContext/WeightsStore/ChainArena/
  ChainRecorder/ChainEngine). Валидация: из CWD=dlss5\m11d\build-nmake
  m11d.exe --selftest C:\Users\AI\Desktop\DLSS5_INTEL\work\_m9b_cmp\native_crop.bmp
  -> build-nmake\out\live_featV.bin/live_head.bin vs work\_m9b_cmp\
  golden_features.bin/golden_head.bin (features 15/16 каналов bit-exact;
  head [TOK,16] ch0-2 vs golden [1088,1344,4] ch0-2 meandiff ~0.0083 = норма).
- Производительность чейна: ~135 мс @ 500x500, ~930 мс @ 1344x1088
  (GEMM-лимит). На игровом окне = slideshow, это ожидаемо. Первая обработка
  кадра у даемона ~6 с (загрузка весов) - не паникуй, не убивай.
- Проверка результата - ТОЛЬКО глазами хозяина в стриме: локальные
  скриншоты Vulkan/flip-окон на этом хосте ЧЁРНЫЕ (GDI и DDA слепы, MPO).

ТЕКУЩЕЕ СОСТОЯНИЕ У ХОЗЯИНА: запущена GTA5 Enhanced (D:\Grand Theft Auto V
Enhanced\GTA5_Enhanced.exe), СЮЖЕТНЫЙ режим, ОКОННЫЙ режим. Enhanced = DX12,
процесс GTA5_Enhanced_BE.exe = BattleEye. Vulkan-слой DX12 не видит.

ЗАДАЧИ (в порядке приоритета):

### M12a (главная): DXGI-перехват DX12-презентов -> m11d
Новый модуль dlss5\m12-dxgi\ (по правилам ООП, см. AGENTS.md). Суть: прокси
dxgi.dll в папку игры (рядом с GTA5_Enhanced.exe), хук IDXGISwapChain::
Present/Present1 (vtable hook после перехвата CreateSwapChain*), внутри:
  1. swapchain->GetBuffer(GetCurrentBackBufferIndex()) -> ID3D12Resource;
  2. копия в readback (свой ID3D12CommandAllocator/List на очереди игры,
     барьеры PRESENT->COPY_SOURCE->PRESENT); на v0 можно медленно;
  3. TCP-раундтрип к m11d ПО ТОМУ ЖЕ протоколу, что у m11-layer (16-байтный
     хедер + BGRA -> ответ); формат бэкбуфера обычно R8G8B8A8_UNORM -
     CPU-свизл R<->B если надо; HDR-форматы (R10G10B10A2/FP16) на v0 -
     логировать и пропускать кадр;
  4. ответ -> upload -> CopyTextureRegion в бэкбуфер (PRESENT->COPY_DEST->
     PRESENT) -> дальше настоящий Present.
  5. НЕ каждый кадр: обрабатывать каждый N-й презент (env M12_LIVE=N,
     default 4), между ними - ре-блит последнего обработанного кадра в
     бэкбуфер (иначе экран мигает необработанным).
Экспорты dxgi: форвардить в системную dxgi.dll (LoadLibrary system32 +
GetProcAddress-проксирование всех CreateDXGIFactory* и т.д.).
BattleEye: только сюжетка; если игра не стартует с прокси - залогировать,
убрать dll, сказать хозяину (тогда fallback = GTA5 Legacy, ниже).
Проверка: лог m12 в %TEMP%\m12_dxgi.log + логи m11d ("frame WxH ...") +
глаза хозяина в стриме. Демон должен быть запущен ДО игры.

### M12b (fallback/параллельно): GTA5 Legacy (DX11) + DXVK x64
DXVK релиз с GitHub -> d3d11.dll+dxgi.dll x64 в папку Legacy рядом с
GTA5.exe -> игра идёт через Vulkan -> уже работающий m11-layer. 32-битный
слой нужен только для DX9-игр (отдельная DLL + манифест library_arch 32),
для GTA5 Legacy x64 НЕ нужен.

### M13 (после M12): менеджер в один клик
Мини-утилита: выбрал папку игры -> сама кладёт нужные DLL (DXVK для DX9-11,
m12-dxgi для DX12), кнопки старт/стоп m11d. Референсы-менеджеры:
reference\DLSS5-Autopilot, DLSS5-Swapper, DLSS-5-MANAGER (чужой код -
только читать). Не начинать, пока M12a/M12b не подтверждены хозяином.

### M10 (если спросит про скорость): тюнинг GEMM
dlss5\m8b-live\shaders\m8\gemm.comp (0.6-2.9 TF/s vs ~30 TF/s железа).
Шейдеры компилятся из m8b-live\shaders обоими потребителями - после правок
пересобрать m11d и прогнать --selftest валидацию (обязательно!).

### Не делать без запроса: миграция m8b-live на dlss5\chain (отдельная
задача, валидация dlss5\m8b-live\_go10.cmd + _go9.cmd).

## /ПРОМПТ (конец)
