
import sys
import struct

def guid_to_bytes(guid_str):
    # UUID format: 2EACA947-7F5F-4CFA-BA87-8F7FBEEFBE69
    parts = guid_str.replace('-', '').replace('{', '').replace('}', '')
    d1 = int(parts[0:8], 16)
    d2 = int(parts[8:12], 16)
    d3 = int(parts[12:16], 16)
    d4s = parts[16:]
    d4 = [int(d4s[i:i+2], 16) for i in range(0, 16, 2)]

    # Struct: DWORD, WORD, WORD, BYTE[8]
    # Little endian for first 3
    return struct.pack('<IHH', d1, d2, d3) + bytes(d4)

def patch_file(input_path, output_path):
    print(f"Reading {input_path}...")
    with open(input_path, 'rb') as f:
        data = f.read()

    # CLSIDs
    org_clsid_str = "2EACA947-7F5F-4CFA-BA87-8F7FBEEFBE69"
    new_clsid_str = "F00DCAFE-0000-0000-0000-000000000001"

    org_bytes = guid_to_bytes(org_clsid_str)
    new_bytes = guid_to_bytes(new_clsid_str)

    print(f"Original GUID Bytes: {org_bytes.hex()}")
    print(f"New GUID Bytes:      {new_bytes.hex()}")

    # 1. Patch RAW GUID BYTES
    count = data.count(org_bytes)
    print(f"Found {count} instances of raw GUID bytes.")
    if count > 0:
        data = data.replace(org_bytes, new_bytes)
        print("Replaced raw GUID bytes.")

    # 2. Patch UTF-16LE String "{GUID}"
    org_wstr = f"{{{org_clsid_str}}}".encode('utf-16le')
    new_wstr = f"{{{new_clsid_str}}}".encode('utf-16le')

    count_w = data.count(org_wstr)
    print(f"Found {count_w} instances of UTF-16LE string.")
    if count_w > 0:
        data = data.replace(org_wstr, new_wstr)
        print("Replaced UTF-16LE strings.")

     # 2b. Patch UTF-16LE String "GUID" (no braces) - Just in case
    org_wstr_nb = f"{org_clsid_str}".encode('utf-16le')
    new_wstr_nb = f"{new_clsid_str}".encode('utf-16le')

    count_w_nb = data.count(org_wstr_nb)
    print(f"Found {count_w_nb} instances of UTF-16LE string (no braces).")
    if count_w_nb > 0:
        # Avoid double replacing if brace version covered it?
        # replace() handles it, but safer to do brace version first (more specific).
        # Check if any left
        if data.count(org_wstr_nb) > 0:
             data = data.replace(org_wstr_nb, new_wstr_nb)
             print("Replaced UTF-16LE strings (no braces).")

    # 3. Patch ASCII String "{GUID}"
    org_astr = f"{{{org_clsid_str}}}".encode('ascii')
    new_astr = f"{{{new_clsid_str}}}".encode('ascii')

    count_a = data.count(org_astr)
    print(f"Found {count_a} instances of ASCII string.")
    if count_a > 0:
        data = data.replace(org_astr, new_astr)
        print("Replaced ASCII strings.")

    # 3b. Patch ASCII String "GUID" (no braces)
    org_astr_nb = f"{org_clsid_str}".encode('ascii')
    new_astr_nb = f"{new_clsid_str}".encode('ascii')

    count_a_nb = data.count(org_astr_nb)
    print(f"Found {count_a_nb} instances of ASCII string (no braces).")
    if count_a_nb > 0:
        data = data.replace(org_astr_nb, new_astr_nb)
        print("Replaced ASCII strings (no braces).")

    print(f"Writing {output_path}...")
    with open(output_path, 'wb') as f:
        f.write(data)
    print("Done.")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: patch_openconsole.py <input> <output>")
        sys.exit(1)

    patch_file(sys.argv[1], sys.argv[2])
