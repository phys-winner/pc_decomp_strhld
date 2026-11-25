"""Share edition-independent lifted code while the edition owns guest state."""
import hashlib
import json
import re
import subprocess


LIBRARY = "libstronghold_common.so"
MODULES = {"mss-x86", "bink-x86"}
HOST = {"android_main.c", "bink_rgb16.c", "map_dcl.c", "miles_audio.c", "miles_upsample.c"}
HOST_CANDIDATES = ("com.c", "crash_journal.c", "dinput8.c", "script.c", "strings.c", "user32.c", "winmm_devices.c")
ENGINES = ("game", "classic", "stronghold")
FUNCTION = re.compile(rb"^void ((?:sub_|ucp_code_)([0-9A-F]{8}))\(void\) \{\r?\n(.*?)^\}", re.M | re.S)
LABEL = re.compile(rb"^(L_[0-9A-F]{8}):", re.M)
TOKEN = re.compile(rb"""/\*.*?\*/|//[^\r\n]*|"(?:\\.|[^"\\\r\n])*"|'(?:\\.|[^'\\\r\n])*'|\bL_[0-9A-F]{8}\b""", re.S)
ENTRY = re.compile(rb"(?m)^    RECOMP_ENTER\(0x([0-9A-F]{8})u\);$")
UNSAFE = re.compile(rb"#|\b(?:static|l_xmm|__func__|__FUNCTION__|__LINE__|__FILE__|__COUNTER__)\b")
CALLEE = re.compile(rb"\b(?:sub_|ucp_code_)[0-9A-F]{8}\w*\b")
DIRECT_CALL = re.compile(rb"(?m)^    RECOMP_CALL\(((?:sub_|ucp_code_)[0-9A-F]{8}\w*)\)")
# shortcut: keep these measured hot helpers local until sharing meets the Honor frame-time baseline
LOCAL_ENGINE_BODIES = {
    b"sh_leaf_076fffc8c52f3fe0bcf7",
    b"sh_leaf_087cf18e4aa02f97aec3",
    b"sh_leaf_157c60964b0640639374",
    b"sh_leaf_2fc156ece23c69640e38",
    b"sh_leaf_57d822f6d3cc782ab732",
    b"sh_leaf_b91c882f4b6e32cea026",
    b"sh_leaf_d346fc612885634aeafc",
    b"sh_leaf_d5eefa6ece61741f0361",
    b"sh_leaf_db36469564c4ebad20b6",
    b"sh_leaf_ec5f3efb9c7467f0b9c4",
    b"sh_leaf_f508af0894b47416aa85",
    b"sh_leaf_ff1ea72419a8c322ecff",
}


def shared_host_sources(clang, folder, flags, edition_flags):
    """Only lift whole host units whose actual preprocessed inputs match."""
    selected = set(HOST)
    for name in HOST_CANDIDATES:
        source = folder / name
        command = [str(clang), *flags, "-E", "-P"]
        neutral = subprocess.check_output([*command, str(source)])
        if all(subprocess.check_output([*command, *extra, str(source)]) == neutral for extra in edition_flags):
            selected.add(name)
        else:
            print(f"[common] {name} differs between build contexts and stays in the editions", flush=True)
    return selected


def leaf(match):
    body = match[3].replace(b"\r\n", b"\n")
    if UNSAFE.search(body):
        return None
    entry = ENTRY.search(body)
    if not entry or entry[1] != match[2]:
        return None
    labels = {name: b"L_%d" % i for i, name in enumerate(LABEL.findall(body))}
    body = ENTRY.sub(b"    RECOMP_ENTER(entry);", body, count=1)
    body = TOKEN.sub(lambda token: b" " if token[0].startswith(b"/") else labels.get(token[0], token[0]), body)
    # shortcut: only explicit RECOMP_CALL operands can be rebound until another lifted reference needs it
    if CALLEE.findall(body) != DIRECT_CALL.findall(body):
        return None
    return b"\n".join(line.rstrip() for line in body.splitlines() if line.strip())


def find_leaves(sources, transform):
    engines = {}
    for bit, module in enumerate(ENGINES):
        for source in sources[module]:
            for match in FUNCTION.finditer(transform(source)):
                if body := leaf(match):
                    engines[body] = engines.get(body, 0) | (1 << bit)
    return {body: mask for body, mask in sorted(engines.items())
            if mask.bit_count() >= 2 and leaf_name(body) not in LOCAL_ENGINE_BODIES}


def leaf_name(body):
    return b"sh_leaf_" + hashlib.sha256(body).hexdigest()[:20].encode()


def replace_leaves(text, leaves):
    def replace(match):
        body = leaf(match)
        if body in leaves:
            name = leaf_name(body)
            callees = sorted(set(DIRECT_CALL.findall(body)))
            params = b", void (*)(void)" * len(callees)
            args = b"".join(b", " + callee for callee in callees)
            return (b"extern void " + name + b"(uint32_t" + params + b");\nvoid " + match[1] +
                    b"(void) { " + name + b"(0x" + match[2] + b"u" + args + b"); }")
        return match[0]
    return FUNCTION.sub(replace, text)


def materialize(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.is_file() or path.read_bytes() != content:
        path.write_bytes(content)
    return path


def leaves_source(path, leaves):
    pieces = [b'#include "host.h"\n#include "display.h"\n#include "touch.h"\n#include "renderer.h"\n']
    for body in leaves:
        callees = {name: b"callee_%d" % index for index, name in enumerate(sorted(set(DIRECT_CALL.findall(body))))}
        params = b"".join(b", void (*" + name + b")(void)" for name in callees.values())
        shared = DIRECT_CALL.sub(lambda call: b"    RECOMP_CALL(" + callees[call[1]] + b")", body)
        pieces.append(b"void " + leaf_name(body) + b"(uint32_t entry" + params + b") {\n" + shared + b"\n}\n")
    return materialize(path, b"".join(pieces))


def symbols(llvm, objects, kind, response):
    materialize(response, "\n".join(str(path).replace("\\", "/") for path in objects).encode())
    out = subprocess.check_output([str(llvm / "llvm-nm.exe"), "--format=posix", "--extern-only",
                                   kind, "@" + str(response)], text=True)
    return {line.split()[0] for line in out.splitlines() if line and not line.endswith(":")}


def verify_group(llvm, shared, editions):
    def exported(path, kind):
        out = subprocess.check_output([str(llvm / "llvm-nm.exe"), "-D", "--format=posix",
                                       "--extern-only", kind, str(path)], text=True)
        return {line.split()[0] for line in out.splitlines() if line}

    common_exports = exported(shared, "--defined-only")
    common_imports = {symbol for symbol in exported(shared, "--undefined-only")
                      if symbol.startswith("stronghold_import_")}
    def dynamic(path):
        report = subprocess.check_output([str(llvm / "llvm-readelf.exe"), "--dynamic", str(path)], text=True)
        return re.findall(r"\((\w+)\)[^\n]*\[([^\]\n]+)\]", report)

    if ("SONAME", LIBRARY) not in dynamic(shared):
        raise RuntimeError(f"{shared.name}: missing common-library SONAME")
    for edition in editions:
        tags = dynamic(edition)
        if ("NEEDED", LIBRARY) not in tags or ("RUNPATH", "$ORIGIN") not in tags:
            raise RuntimeError(f"{edition.name}: missing common dependency or $ORIGIN runpath")
        imports = {symbol for symbol in exported(edition, "--undefined-only")
                   if symbol.startswith(("sub_", "sh_leaf_", "void_", "stronghold_", "recomp_"))}
        missing = ((imports - common_exports) |
                   (common_imports - exported(edition, "--defined-only")))
        if missing:
            raise RuntimeError(f"{edition.name}: unresolved shared dependencies: {sorted(missing)}")


def link_record_path(edition):
    return edition.with_suffix(".common.json")


def record_link(edition, shared, sha):
    common_key, common_sha = (shared.parent / "common/linked.sha256").read_text().split()
    if sha(shared) != common_sha:
        raise RuntimeError("Common library changed during the native build")
    record = {"edition": edition.name, "sha256": sha(edition),
              "common_sha256": common_sha, "common_build": common_key}
    materialize(link_record_path(edition), (json.dumps(record, indent=2) + "\n").encode())


def verify_link_records(shared, editions, sha):
    verified = {shared.name: sha(shared)}
    key = None
    for edition in editions:
        path = link_record_path(edition)
        if not path.is_file():
            raise ValueError(f"{edition.name}: missing common link record; rebuild the native libraries")
        record = json.loads(path.read_text(encoding="utf-8"))
        fingerprint = sha(edition)
        build = record.get("common_build")
        if (record.get("edition") != edition.name or record.get("sha256") != fingerprint
                or record.get("common_sha256") != verified[shared.name]
                or not isinstance(build, str) or not re.fullmatch(r"[0-9a-f]{64}", build)
                or (key is not None and build != key)):
            raise ValueError(f"{edition.name}: library does not match the common link records; rebuild all editions")
        key = build
        verified[edition.name] = fingerprint
    return verified


def link_common(llvm, clang, target, output, common, edition, sha, profile_generate):
    work = output / "common"
    response = work / "nm.rsp"
    undefined = symbols(llvm, common, "--undefined-only", response)
    defined = symbols(llvm, common, "--defined-only", response)
    if not any(symbol.startswith("sh_leaf_") for symbol in defined):
        raise RuntimeError("The common library is missing the deduplicated engine bodies")
    imports = sorted((undefined - defined) & symbols(llvm, edition, "--defined-only", response))
    if not {"g_eax", "g_esp", "void_vm_readable"} <= set(imports):
        raise RuntimeError("The common library is missing its edition bridge")
    aliases = "".join(f"{symbol} stronghold_import_{symbol}\n" for symbol in imports)
    library = output / LIBRARY
    fingerprint = work / "linked.sha256"
    key = hashlib.sha256((aliases + sha(clang) + str(profile_generate) +
                          sha(__file__) + "".join(sha(obj) for obj in common)).encode()).hexdigest()
    if not (library.is_file() and fingerprint.is_file() and
            fingerprint.read_text() == key + " " + sha(library)):
        symbol_map = materialize(work / "imports.txt", aliases.encode())
        rewritten = []
        for obj in common:
            path = work / (obj.parent.name + "-" + obj.name)
            subprocess.run([str(llvm / "llvm-objcopy.exe"), "--redefine-syms=" + str(symbol_map),
                            str(obj), str(path)], check=True)
            rewritten.append(path)
        link_input = materialize(work / "link.rsp", "\n".join(path.as_posix() for path in rewritten).encode())
        cmd = [str(clang), "--target=" + target, "-shared", "-Wl,-soname," + LIBRARY,
               "-Wl,-z,max-page-size=16384,-z,common-page-size=16384",
               "@" + str(link_input), "-o", str(library), "-llog", "-ldl", "-lm", "-lz",
               "-landroid", "-laaudio", "-lOpenSLES", "-ljnigraphics"]
        if profile_generate:
            cmd.append("-fprofile-instr-generate")
        subprocess.run(cmd, check=True)
        fingerprint.write_text(key + " " + sha(library))
    bridge = [f"-Wl,--defsym=stronghold_import_{symbol}={symbol},--export-dynamic-symbol=stronghold_import_{symbol}"
              for symbol in imports]
    return [str(library), "-Wl,--allow-shlib-undefined,-rpath,$ORIGIN", *bridge]
