# ============================================================================
# m13._build_production - assemble a self-contained production bundle the
# owner can run from anywhere (default: C:\Users\AI\Desktop\production).
#
#   python _build_production.py [dest_dir]
#
# Copies: this manager package (whitelist), the runtime artifacts (m11d +
# spv, m8blive + spv, x86/x64 layer dlls + manifests, DXVK x32 d3d9.dll,
# m12 proxy), patches the manifests' library_path to the DEST location
# (x86 loader requires an ABSOLUTE path), copies the model weights, and
# verifies sha1 for every copied binary. ASCII only, stdlib only.
#
# The weights are PROPRIETARY - the bundle must never be committed to git.
# ============================================================================
import hashlib
import json
import os
import shutil
import sys

_PKG = os.path.dirname(os.path.abspath(__file__))          # dlss5\m13
_DLSS5 = os.path.dirname(_PKG)
_REPO = os.path.dirname(_DLSS5)
DEST = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else \
    os.path.join(os.path.expanduser("~"), "Desktop", "production")
WEIGHTS_SRC = os.path.join(_REPO, "work", "mlxw",
                           "dlssnr-logical.safetensors")

PKG_FILES = ["__init__.py", "config.py", "daemonctl.py", "deploy.py",
             "gamelaunch.py", "gamescan.py", "icons.py", "logtail.py",
             "overlay.py", "paths.py", "processes.py", "screenmode.py",
             "splash.py", "controller.py", "ui.py", "ui_games.py", "m13.pyw"]

# (source, dest-relative, glob-ish file list)
ARTIFACTS = [
    (os.path.join(_DLSS5, "m11d", "build-nmake"), ("runtime", "m11d"),
     ["m11d.exe", "*.spv"]),
    (os.path.join(_DLSS5, "m8b-live", "build", "Release"), ("runtime", "m8b"),
     ["m8blive.exe", "*.spv"]),
    (os.path.join(_DLSS5, "m11-layer", "build", "Release"),
     ("runtime", "layer", "x64"),
     ["nr_layer_win.dll", "VkLayer_dlssnr_win.json"]),
    (os.path.join(_DLSS5, "m11-layer", "x86"), ("runtime", "layer", "x86"),
     ["nr_layer_win32.dll", "VkLayer_dlssnr_win32.json"]),
    (os.path.join(_DLSS5, "m11-layer", "dxvk", "x32"),
     ("runtime", "dxvk", "x32"),
     ["d3d9.dll", "d3d11.dll", "d3d10core.dll"]),
    (os.path.join(_DLSS5, "m11-layer", "dxvk", "x64"),
     ("runtime", "dxvk", "x64"),
     ["d3d9.dll", "d3d11.dll", "d3d10core.dll", "dxgi.dll"]),
    (os.path.join(_DLSS5, "m12-dxgi", "build", "Release"),
     ("runtime", "m12"), ["m12_dxgi.dll"]),
]

README = """DLSS 5 Manager - production bundle
==================================
Запуск: M13.cmd

Всё само: при старте менеджер находит веса, регистрирует слои Vulkan,
запускает демон и сканирует диски. Сканер показывает ТОЛЬКО реальные игры
(иконки-карточки), а не каждый exe на диске. Игры с лаунчером (Stalker 2,
RDR2) определяются сами: DLSS ставится на настоящий exe игры, а запуск
идёт через её лаунчер.

Как играть:
  1. Вкладка "Игры" - выбери карточку игры (или "Добавить exe вручную").
  2. "Включить DLSS 5" - ставит нужные DLL под API игры
     (DX9/DX10/DX11 через DXVK->Vulkan слой, DX12 через наш dxgi-прокси).
     Режим определяется автоматически; можно переопределить выпадайкой.
     Защищённые папки (Program Files) спросят UAC один раз.
  3. "Играть". В игре CTRL+ALT+G открывает панель ПОВЕРХ игры:
     кадр ЗАМОРАЖИВАЕТСЯ, крути ползунок силы - замороженный кадр
     переобрабатывается из исходника с новыми настройками. Закрыл панель -
     игра продолжается уже с новой силой эффекта.

Вкладка "Экран": полноэкранный оверлей рабочего стола. ВАЖНО: демон и
оверлей экрана не работают одновременно (каждому нужно ~12 ГБ из 16 ГБ
видеопамяти) - менеджер сам останавливает одно перед запуском другого.
Прогрев 1440p: ~30 с загрузка весов, до минуты первый кадр.

Горячие клавиши слоя/прокси (в игре): CTRL+ALT+X пауза/продолжить
обработку, CTRL+ALT+Q выключить слой.

АНТИЧИТ: не включай в онлайн-играх (PUBG, CS2, GTA Online) - внедрение
DLL могут посчитать читом.

Логи - в нижней панели. Конфиг: %LOCALAPPDATA%\\DLSS5Manager\\config.json
"""


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def copy_artifacts():
    copied = []
    for src_dir, rel, patterns in ARTIFACTS:
        dst_dir = os.path.join(DEST, *rel)
        os.makedirs(dst_dir, exist_ok=True)
        for pat in patterns:
            if pat.startswith("*."):
                names = [n for n in os.listdir(src_dir) if n.endswith(pat[1:])]
            else:
                names = [pat] if os.path.exists(os.path.join(src_dir, pat)) \
                    else []
            for name in names:
                s, d = os.path.join(src_dir, name), os.path.join(dst_dir, name)
                shutil.copy2(s, d)
                assert sha1(s) == sha1(d), "hash mismatch: %s" % name
                copied.append(d)
    return copied


def patch_manifests():
    """library_path must be ABSOLUTE and point at THIS bundle (x86 loader
    quirk: relative paths fail with error 87)."""
    for rel, dll in ((("runtime", "layer", "x64", "VkLayer_dlssnr_win.json"),
                      ("runtime", "layer", "x64", "nr_layer_win.dll")),
                     (("runtime", "layer", "x86",
                       "VkLayer_dlssnr_win32.json"),
                      ("runtime", "layer", "x86", "nr_layer_win32.dll"))):
        mp = os.path.join(DEST, *rel)
        with open(mp, "r") as f:
            doc = json.load(f)
        doc["layer"]["library_path"] = os.path.join(DEST, *dll)
        with open(mp, "w") as f:
            json.dump(doc, f, indent=4)


def main():
    if not os.path.exists(WEIGHTS_SRC):
        raise SystemExit("weights not found: %s" % WEIGHTS_SRC)
    os.makedirs(DEST, exist_ok=True)
    os.makedirs(os.path.join(DEST, "logs"), exist_ok=True)
    os.makedirs(os.path.join(DEST, "weights"), exist_ok=True)

    pkg_dst = os.path.join(DEST, "m13")
    os.makedirs(pkg_dst, exist_ok=True)
    for name in PKG_FILES:
        shutil.copy2(os.path.join(_PKG, name), os.path.join(pkg_dst, name))

    n = copy_artifacts()
    patch_manifests()

    w_dst = os.path.join(DEST, "weights",
                         os.path.basename(WEIGHTS_SRC))
    if not os.path.exists(w_dst):
        shutil.copy2(WEIGHTS_SRC, w_dst)
    assert sha1(WEIGHTS_SRC) == sha1(w_dst), "weights hash mismatch"

    with open(os.path.join(DEST, "M13.cmd"), "w", newline="\r\n") as f:
        f.write('@echo off\r\n'
                'start "DLSS5 Manager" /min '
                '"C:\\Users\\AI\\AppData\\Local\\Programs\\Python\\'
                'Python312\\pythonw.exe" "%~dp0m13\\m13.pyw"\r\n')
    with open(os.path.join(DEST, "README.txt"), "w", newline="\r\n",
              encoding="utf-8") as f:
        f.write(README.replace("\n", "\r\n"))

    total = sum(os.path.getsize(p) for p in
                [os.path.join(DEST, "weights", os.path.basename(WEIGHTS_SRC))]
                + n)
    print("bundle at %s" % DEST)
    print("binaries copied+hash-verified: %d, weights ok (%d bytes total)"
          % (len(n), total))


if __name__ == "__main__":
    main()
