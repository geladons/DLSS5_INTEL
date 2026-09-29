import struct
import os

def get_exports(filepath):
    with open(filepath, "rb") as f:
        data = f.read()
    if data[:2] != b"MZ":
        return []
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew+4] != b"PE\x00\x00":
        return []
    magic = struct.unpack_from("<H", data, e_lfanew + 24)[0]
    is_64 = (magic == 0x20b)
    
    num_sections = struct.unpack_from("<H", data, e_lfanew + 6)[0]
    opt_header_size = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    sec_table_offset = e_lfanew + 24 + opt_header_size
    
    # Export directory in DataDirectory[0]
    # For PE32, DataDirectory starts at e_lfanew + 24 + 96 = e_lfanew + 120
    # For PE32+, DataDirectory starts at e_lfanew + 24 + 112 = e_lfanew + 136
    data_dir_offset = e_lfanew + 24 + (112 if is_64 else 96)
    export_rva, export_size = struct.unpack_from("<II", data, data_dir_offset)
    if export_rva == 0:
        return []
        
    sections = []
    for i in range(num_sections):
        sec_off = sec_table_offset + i * 40
        name = data[sec_off:sec_off+8].rstrip(b"\x00").decode("latin-1")
        vsize, va, rsize, rptr = struct.unpack_from("<IIII", data, sec_off + 8)
        sections.append((va, vsize, rptr, rsize))
        
    def rva_to_offset(rva):
        for va, vsize, rptr, rsize in sections:
            if va <= rva < va + max(vsize, rsize):
                return rptr + (rva - va)
        return None

    exp_off = rva_to_offset(export_rva)
    if not exp_off:
        return []
    
    num_names = struct.unpack_from("<I", data, exp_off + 24)[0]
    names_rva = struct.unpack_from("<I", data, exp_off + 32)[0]
    names_off = rva_to_offset(names_rva)
    if not names_off:
        return []
        
    names = []
    for i in range(num_names):
        name_rva = struct.unpack_from("<I", data, names_off + i * 4)[0]
        n_off = rva_to_offset(name_rva)
        if n_off:
            s_end = data.find(b"\x00", n_off)
            names.append(data[n_off:s_end].decode("ascii", errors="replace"))
    return names

dlls = [
    r"C:\Users\<owner>\Desktop\production\runtime\layer\x64\nr_layer_win.dll",
    r"C:\Users\<owner>\Desktop\production\runtime\layer\x86\nr_layer_win32.dll",
    r"C:\Users\<owner>\Desktop\production\runtime\m12\m12_dxgi.dll",
    r"C:\Users\<owner>\Desktop\production\runtime\m11d\d5c_chain.dll",
]

for d in dlls:
    print(f"=== {os.path.basename(d)} ===")
    exps = get_exports(d)
    print(f"  Count: {len(exps)}")
    for e in exps[:20]:
        print(f"    {e}")
    if len(exps) > 20:
        print(f"    ... and {len(exps)-20} more")
