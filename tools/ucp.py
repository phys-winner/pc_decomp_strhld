"""Prepare pinned UCP Lua patches for the Android AOT build; upstream stays unmodified."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

import pefile

ROOT = Path(__file__).resolve().parents[1]
PIN = json.loads((ROOT / 'dependencies.json').read_text())['ucp']
UPSTREAM = ROOT / PIN['directory']
FRAMEWORK = UPSTREAM / 'UCP3'
LEGACY = FRAMEWORK / 'content/ucp/modules/ucp2-legacy-2.15.1'
OUT = ROOT / 'generated/ucp'
ADAPTATION = json.loads((ROOT / 'config/ucp-android.json').read_text())


def checkout(fetch=False):
    if fetch:
        if not UPSTREAM.exists():
            subprocess.run(['git', 'clone', '--no-checkout', PIN['url'], str(UPSTREAM)], check=True)
        subprocess.run(['git', '-C', str(UPSTREAM), 'checkout', '--detach', PIN['revision']], check=True)
        subprocess.run(['git', '-C', str(UPSTREAM), 'submodule', 'update', '--init', 'UCP3'], check=True)
        subprocess.run(['git', '-C', str(FRAMEWORK), 'submodule', 'update', '--init',
                        'content/ucp/modules/ucp2-legacy-2.15.1',
                        'content/ucp/plugins/ucp2-legacy-defaults-2.15.1'], check=True)
    for directory, revision in [(UPSTREAM, PIN['revision']), (FRAMEWORK, PIN['framework_revision']),
                                (LEGACY, PIN['legacy_revision'])]:
        result = subprocess.run(['git', '-C', str(directory), 'rev-parse', 'HEAD'],
                                capture_output=True, text=True, check=True)
        if result.stdout.strip() != revision:
            raise ValueError(f'{directory.name}: UCP revision mismatch; run tools/ucp.py --fetch')
        dirty = subprocess.run(['git', '-C', str(directory), 'status', '--porcelain', '--untracked-files=no', '--ignore-submodules'],
                               capture_output=True, text=True, check=True)
        if dirty.stdout.strip():
            raise ValueError(f'{directory.name}: upstream sources were modified')


def options():
    import yaml
    document = yaml.safe_load((LEGACY / 'options.yml').read_text(encoding='utf-8'))
    config = {}

    def visit(value):
        if isinstance(value, list):
            for child in value:
                visit(child)
        elif isinstance(value, dict):
            url = value.get('url', '')
            if url.startswith('ucp2-legacy.'):
                path = url.split('.')[1:]
                target = config
                for key in path[:-1]:
                    target = target.setdefault(key, {})
                target[path[-1]] = copy.deepcopy(value['contents']['value'])
            for child in value.values():
                visit(child)
    visit(document['options'])

    def enable(value):
        for key, child in value.items():
            if key == 'enabled':
                value[key] = True
            elif isinstance(child, dict):
                enable(child)
    enable(config)
    config['ai_attacktarget']['choice'] = 'nearest'
    for name, values in ADAPTATION['feature_overrides'].items():
        if name not in config or not isinstance(config[name], dict):
            raise ValueError(f'Unknown Android UCP feature: {name}')
        config[name].update(values)
    return config


def catalog_metadata(catalog):
    """Keep presentation metadata separate from patch order and native data."""
    import yaml
    categories = {
        ('ai', 'fixes'): ('ai_fixes', 'ИИ · исправления'),
        ('ai', 'improvements'): ('ai_improvements', 'ИИ · улучшения'),
        ('bugfixes',): ('bugfixes', 'Исправления ошибок'),
        ('balance_changes',): ('balance_changes', 'Баланс'),
        ('qol',): ('qol', 'Удобство'),
        ('misc',): ('misc', 'Прочее'),
    }
    paths = {}

    def visit(value, inherited=()):
        if isinstance(value, list):
            for child in value:
                visit(child, inherited)
        elif isinstance(value, dict):
            category = tuple(item.strip('{}') for item in value.get('category', inherited))
            url = value.get('url', '')
            if url.startswith('ucp2-legacy.'):
                feature = url.split('.')[1]
                if category not in categories or feature in paths and paths[feature] != category:
                    raise ValueError(f'Ambiguous UCP category: {feature}')
                paths[feature] = category
            for child in value.values():
                visit(child, category)

    visit(yaml.safe_load((LEGACY / 'options.yml').read_text(encoding='utf-8'))['options'])
    credits = json.loads((ROOT / 'config/ucp-credits.json').read_text(encoding='utf-8'))['features']
    ids = {feature['id'] for feature in catalog['features']}
    if not set(credits) <= ids:
        raise ValueError('Credits reference an unknown UCP feature')
    catalog['categories'] = [{'id': ident, 'name': name} for ident, name in categories.values()]
    for feature in catalog['features']:
        feature['category'] = categories[paths[feature['id']]][0]
        authors = credits.get(feature['id'], [])
        if any(not author['name'].strip() or not author['source'].startswith('https://github.com/UnofficialCrusaderPatch/') for author in authors):
            raise ValueError(f'Invalid UCP attribution: {feature["id"]}')
        feature['authors'] = authors


def update_catalog_metadata():
    path = OUT / 'catalog.json'
    catalog = json.loads(path.read_text(encoding='utf-8'))
    if catalog['upstream'] != PIN:
        raise ValueError('UCP catalog revision mismatch')
    catalog_metadata(catalog)
    path.write_text(json.dumps(catalog, ensure_ascii=False, indent=2), encoding='utf-8')


class PatchMemory:
    """A checked PE image and deterministic guest allocations, never host executable memory."""
    def __init__(self, binary, index):
        self.pe = pefile.PE(data=binary)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.original = self.pe.get_memory_mapped_image()
        self.image = bytearray(self.original)
        self.next_address = 0x50000000 + index * 0x100000
        self.allocations = {}
        self.writes = []
        self.scans = []
        self.lua = None

    def region(self, address, size):
        if size < 0 or size > 0x100000:
            raise ValueError('Unbounded UCP memory access')
        if self.base <= address and address + size <= self.base + len(self.image):
            return self.image, address - self.base
        for base, block in self.allocations.items():
            if base <= address and address + size <= base + len(block['data']):
                return block['data'], address - base
        raise ValueError(f'UCP accesses unmapped memory {address:#x}+{size:#x}')

    def read(self, address, size):
        region, offset = self.region(address, size)
        return bytes(region[offset:offset + size])

    def write(self, address, values):
        data = bytes(values[i] for i in range(1, len(values) + 1))
        region, offset = self.region(address, len(data))
        before = bytes(region[offset:offset + len(data)])
        region[offset:offset + len(data)] = data
        self.writes.append((address, before.hex(), data.hex()))

    def allocate(self, size, code=False):
        if not 0 < size <= 0x80000:
            raise ValueError(f'Invalid UCP allocation size: {size}')
        address = self.next_address
        self.next_address += (size + 15) & ~15
        self.allocations[address] = {'code': code, 'data': bytearray(size)}
        return address

    def scan(self, pattern, start=None, stop=None):
        effective = ADAPTATION['aob_patterns'].get(pattern, pattern)
        tokens = effective.split()
        expression = b''.join(b'.' if token in ('?', '??') else re.escape(bytes([int(token, 16)])) for token in tokens)
        lo = max(self.base, self.base if start is None else start)
        hi = min(self.base + len(self.original), 0x7fffffff if stop is None else stop)
        match = re.search(expression, self.original[lo - self.base:hi - self.base], re.DOTALL)
        if match is None:
            raise ValueError(f'UCP AOB not found: {pattern}')
        if effective != pattern and re.search(expression, self.original[lo - self.base + match.end():hi - self.base], re.DOTALL):
            raise ValueError(f'Android UCP signature is ambiguous: {pattern}')
        address = lo + match.start()
        self.scans.append((address, pattern))
        return address

    def execute(self, feature, config):
        from lupa.lua54 import LuaRuntime
        lua = self.lua = LuaRuntime(unpack_returned_tuples=True)
        lua.globals().package.path = str(FRAMEWORK / 'content/ucp/code/?.lua').replace('\\', '/') + ';' + \
                                    str(LEGACY / '?.lua').replace('\\', '/')
        internal = lua.table()
        internal.readInteger = lambda address: int.from_bytes(self.read(address, 4), 'little', signed=True)
        internal.readSmallInteger = lambda address: int.from_bytes(self.read(address, 2), 'little')
        internal.readByte = lambda address: self.read(address, 1)[0]
        internal.readBytes = lambda address, size: lua.table_from(self.read(address, size))
        internal.writeCode = self.write
        internal.writeBytes = self.write
        internal.allocate = lambda size, zero=None: self.allocate(size)
        internal.allocateCode = lambda size: self.allocate(size, True)
        internal.scanForAOB = self.scan
        for name, size in [('writeInteger', 4), ('writeSmallInteger', 2), ('writeByte', 1)]:
            internal[name] = lambda address, value, size=size: self.write(address, lua.table_from(
                (int(value) & ((1 << (size * 8)) - 1)).to_bytes(size, 'little')))
        lua.globals().ucp = lua.table_from({'internal': internal})
        lua.globals().log = lambda *args: None
        lua.globals().DEBUG = 0
        lua.globals().INFO = 1
        lua.globals().WARNING = 2
        lua.globals().data = lua.table_from({'cache': lua.table_from({'AOB': lua.table_from({'retrieve': self.scan})})})
        lua.execute("core = require('core'); utils = require('utils')")
        module = lua.execute((LEGACY / 'port' / (feature + '.lua')).read_text(encoding='utf-8'))
        value = lua.table_from(config, recursive=True)
        module.init(module, value)
        module.enable(module, value)

    def result(self):
        return {'writes': self.writes, 'scans': self.scans,
                'allocations': [{'address': a, 'code': v['code'], 'data': bytes(v['data']).hex()}
                                for a, v in self.allocations.items()]}


def extract():
    configurations = options()
    OUT.mkdir(parents=True, exist_ok=True)
    records = {}
    inputs = json.loads((ROOT / 'config/inputs.json').read_text())
    for edition, executable in [('classic', 'Stronghold Crusader.exe'), ('extreme', 'Stronghold_Crusader_Extreme.exe')]:
        binary = (ROOT / 'generated/input' / executable).read_bytes()
        if hashlib.sha256(binary).hexdigest() != inputs[executable]:
            raise ValueError(f'{executable}: input SHA256 mismatch')
        features = {}
        for index, (feature, config) in enumerate(sorted(configurations.items())):
            memory = PatchMemory(binary, index)
            try:
                memory.execute(feature, config)
                features[feature] = {'config': config, **memory.result()}
                print(f'{edition}: {feature}: {len(memory.writes)} writes', flush=True)
            except Exception as error:
                features[feature] = {'error': str(error)}
                print(f'{edition}: {feature}: UNAVAILABLE: {error}', flush=True)
        records[edition] = features
        (OUT / f'{edition}-patches.json').write_text(json.dumps(features, indent=2), encoding='utf-8')
    return records


def merge_ranges(ranges):
    merged = []
    for start, end in sorted(ranges):
        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(end, merged[-1][1]))
        else:
            merged.append((start, end))
    return merged


def patch_areas(original, image, patched, base, md):
    """End at an instruction boundary shared by the original and patched code."""
    from tools.disasm.disasm32 import Instruction
    changed = [(i.address, i.end_address) for i in original
               if image[i.address-base:i.end_address-base] != patched[i.address-base:i.end_address-base]]
    boundaries = {i.address for i in original} | {i.end_address for i in original}
    areas = []
    for start, end in merge_ranges(changed):
        if areas and end <= areas[-1][1]:
            continue
        if areas and start < areas[-1][1]:
            start = areas.pop()[0]
        instructions = []
        cursor = start
        while cursor < end or cursor not in boundaries:
            i = next(md.disasm(bytes(patched[cursor-base:cursor-base+15]), cursor, count=1), None)
            if i is None or cursor > end + 32:
                raise ValueError(f'Patched instructions do not rejoin the original at {cursor:#x}')
            instructions.append(Instruction(i.address, i.size, i.mnemonic, i.op_str, bytes(i.bytes), list(i.operands)))
            cursor += i.size
        areas.append((start, cursor, instructions))
    return areas


def walk_code(md, segments, entries):
    """Follow executable paths; injected blocks can also contain skipped data bytes."""
    from tools.disasm.disasm32 import Instruction
    result, occupied, pending = {}, {}, list(entries)
    while pending:
        pc = pending.pop()
        if pc in result:
            continue
        block = next(((start, data) for start, data in segments if start <= pc < start + len(data)), None)
        if block is None:
            continue
        start, data = block
        native = next(md.disasm(data[pc-start:pc-start+15], pc, count=1), None)
        if native is None or pc in occupied:
            raise ValueError(f'Invalid or overlapping injected instruction at {pc:#x}')
        instruction = Instruction(native.address, native.size, native.mnemonic, native.op_str, bytes(native.bytes), list(native.operands))
        for byte in range(pc, instruction.end_address):
            if byte in occupied:
                raise ValueError(f'Overlapping injected instruction at {pc:#x}')
            occupied[byte] = pc
        result[pc] = instruction
        if instruction.is_jump or instruction.is_call:
            target = instruction.get_branch_target()
            if target is not None:
                pending.append(target)
        if not instruction.is_ret and not instruction.is_uncond_jump:
            pending.append(instruction.end_address)
    return sorted(result.values(), key=lambda i: i.address)


def generate(records):
    """Translate the changed instructions and x86 trampolines with the existing checked lifter."""
    import capstone
    import lift_core
    from tools.disasm.disasm32 import Instruction

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True

    class HookLifter(lift_core.CheckedLifter):
        def __init__(self, *args, guards=None, exits=None, external=(), labels=(), jump_targets=None, **kwargs):
            super().__init__(*args, **kwargs)
            self.guards, self.exits, self.external = guards or {}, exits or {}, set(external)
            self._labels = set(labels) | self.external
            self._jump_targets = set(labels if jump_targets is None else jump_targets)

        def lift_instruction(self, instruction):
            self._labels.update(self.external)
            if instruction.address in self.exits:
                return [f'goto ORIGINAL_{self.exits[instruction.address]:08X};']
            return self.guards.get(instruction.address, []) + super().lift_instruction(instruction)

    catalog = {'upstream': PIN, 'features': []}
    import yaml
    locale = yaml.safe_load((LEGACY / 'locale/ru.yml').read_text(encoding='utf-8'))
    ids = sorted(records['classic'])
    for index, name in enumerate(ids):
        catalog['features'].append({'id': name, 'index': index, 'name': locale.get(name, name),
                                    'description': locale.get(name + '_descr', ''), 'editions': {}})
    for edition, module in [('classic', 'classic'), ('extreme', 'game')]:
        binary = (ROOT / 'generated/input' / ('Stronghold Crusader.exe' if edition == 'classic' else 'Stronghold_Crusader_Extreme.exe')).read_bytes()
        pe = pefile.PE(data=binary)
        base = pe.OPTIONAL_HEADER.ImageBase
        image = pe.get_memory_mapped_image()
        info = lift_core.analyze_pe(ROOT / 'generated/input' / ('Stronghold Crusader.exe' if edition == 'classic' else 'Stronghold_Crusader_Extreme.exe'))
        imports = lift_core.build_iat_map(info)
        decoder = lift_core.BoundedDisassembler(binary, base, info.sections)
        entries = {int(a, 16) for a in re.findall(r'void sub_([0-9A-F]+)\(void\);',
                   (ROOT / f'generated/{module}-x86/recomp_funcs.h').read_text())}
        functions, owners, incoming = {}, {}, set(entries)
        for source in sorted((ROOT / f'generated/{module}-x86').glob('recomp_*.c')):
            text = source.read_text()
            for match in re.finditer(r'(?m)^void sub_([0-9A-F]{8})\(void\) \{.*?^\}', text, re.DOTALL):
                address, body = int(match[1], 16), match[0]
                pcs = {int(a, 16) for a in re.findall(r'/\* 0x([0-9A-F]{8}):', body)}
                if not pcs:
                    continue
                # Generated C omits PC comments on indirect jumps. Reuse the
                # original decoder rather than treating comments as assembly.
                decoded = decoder.disassemble_function(address, imports)
                instructions = sorted({i.address: i for block in decoded.blocks.values()
                                       for i in block.instructions}.values(), key=lambda i:i.address)
                if not pcs <= {i.address for i in instructions}:
                    raise ValueError(f'Original function coverage changed at {address:#x}')
                pcs = {i.address for i in instructions}
                functions[address] = {'body': body, 'pcs': pcs, 'instructions': instructions,
                                      'jump_targets': set(decoded.jump_targets)}
                incoming.update(decoded.jump_targets)
                for pc in pcs:
                    instruction = next(md.disasm(image[pc-base:pc-base+15], pc, count=1), None)
                    if instruction is None:
                        raise ValueError(f'Original instruction cannot be decoded: {pc:#x}')
                    if (instruction.group(capstone.CS_GRP_JUMP) or instruction.group(capstone.CS_GRP_CALL)) and instruction.operands:
                        operand = instruction.operands[0]
                        if operand.type == capstone.x86.X86_OP_IMM:
                            incoming.add(operand.imm & 0xffffffff)
                    for byte in range(pc, pc + instruction.size):
                        owners.setdefault(byte, set()).add(address)
        changes, allocations, active_records = {}, {}, {}
        for index, name in enumerate(ids):
            record = records[edition][name]
            feature = catalog['features'][index]['editions']
            if 'error' in record:
                feature[edition] = {'error': record['error']}
                continue
            patched = bytearray(image)
            feature_owners = set()
            for address, before, after in record['writes']:
                if base <= address < base + len(image):
                    data = bytes.fromhex(after)
                    patched[address-base:address-base+len(data)] = data
                    for byte in range(address, address + len(data)):
                        feature_owners.update(owners.get(byte, ()))
            try:
                feature_changes = {}
                for owner in sorted(feature_owners):
                    function = functions[owner]
                    if 'RECOMP_EH_ENTER' in function['body']:
                        raise ValueError(f'Patch enters an exception-handling function {owner:#x}')
                    original = function['instructions']
                    areas = patch_areas(original, image, patched, base, md)
                    if not areas:
                        continue
                    segments = [(block['address'], bytes.fromhex(block['data'])) for block in record['allocations'] if block['code']]
                    code = walk_code(md, segments, [address for address, _ in segments])
                    areas = [(start, end, walk_code(md, [(start, bytes(patched[start-base:end-base]))],
                              {start} | (incoming & set(range(start, end))))) for start, end, _ in areas]
                    all_patch = code + [i for _, _, instructions in areas for i in instructions]
                    external = set(function['pcs'])
                    required_labels = {i.get_branch_target() for i in all_patch if i.is_jump and i.get_branch_target() in external}
                    required_labels.update(end for _, end, _ in areas if end in external)
                    guards = {}
                    snippets = []
                    for number, (start, end, instructions) in enumerate(areas):
                        prefix = f'UCP_{index}_{number}_'
                        local_pcs = {i.address for i in instructions + code}
                        exits = {end: end}
                        sentinel = Instruction(end, 1, 'nop', '', b'\x90', [])
                        body_instructions = sorted(instructions + [sentinel] + code, key=lambda i: i.address)
                        lifter = HookLifter(iat_map=imports, lifted=entries, precise_sbb=True, precise_carry=True,
                                           exits=exits, external=external, labels={i.address for i in body_instructions})
                        snippet = lift_core.lift_function_linear(lifter, 'ucp_fragment', body_instructions,
                            {i.address for i in body_instructions}, start, indirect_targets=None)
                        marker = f'    RECOMP_ENTER(0x{start:08X}u);'
                        if marker not in snippet:
                            raise ValueError('UCP fragment preamble changed')
                        snippet = snippet.split(marker, 1)[1].rsplit('\n}', 1)[0]
                        snippet = re.sub(r'(?m)^[ \t]*L_([0-9A-F]{8}): RECOMP_ITAIL\([^\n]*\n?',
                            lambda m: '' if int(m[1],16) in external and int(m[1],16) not in local_pcs | {end} else m[0], snippet)
                        own_labels = {int(a,16) for a in re.findall(r'(?m)^[ \t]*L_([0-9A-F]{8}):', snippet)}
                        snippet = re.sub(r'\bL_([0-9A-F]{8})', lambda m: prefix+'L_'+m[1] if int(m[1],16) in own_labels else m[0], snippet)
                        snippet = re.sub(r'\b_ljump\b', prefix+'_ljump', snippet)
                        snippet = re.sub(r'ORIGINAL_([0-9A-F]{8})', r'L_\1', snippet)
                        if 'void_lift_trap(' in snippet or 'UNIMPLEMENTED' in snippet:
                            traps = [line.strip() for line in snippet.splitlines() if 'void_lift_trap(' in line or 'UNIMPLEMENTED' in line]
                            raise ValueError(f'Unsupported instruction in {name} at {start:#x}: {traps[0][:180]}')
                        for pc in function['pcs'] & set(range(start, end)):
                            if pc == start or pc in incoming:
                                if pc not in local_pcs:
                                    raise ValueError(f'Overwritten branch entry is not an instruction: {pc:#x}')
                                guards.setdefault(pc, []).append(f'if (stronghold_ucp_enabled[{index}]) goto {prefix}L_{pc:08X};')
                        snippets.append(snippet)
                    feature_changes[owner] = {'original': original, 'guards': guards, 'snippets': snippets, 'labels': required_labels,
                                              'ranges': [(a, b) for a, b, _ in areas]}
                for owner, change in feature_changes.items():
                    changes.setdefault(owner, []).append(change)
                record['code_ranges'] = sorted({tuple(area) for change in feature_changes.values() for area in change['ranges']})
                active_records[index] = record
                allocations.update({block['address']: block for block in record['allocations']})
                feature[edition] = {'available': True, 'config': record['config'], 'writes': len(record['writes'])}
            except Exception as error:
                feature[edition] = {'error': str(error)}
                print(f'{edition}: {name}: AOT UNAVAILABLE: {error}', flush=True)
        replacements = {}
        for owner, modified in changes.items():
            function = functions[owner]
            guards = {}
            for change in modified:
                for pc, lines in change['guards'].items():
                    guards.setdefault(pc, []).extend(lines)
            leaders = {int(a, 16) for a in re.findall(r'(?m)^L_([0-9A-F]{8}):', function['body'])}
            leaders.update(pc for change in modified for pc in change['labels'])
            leaders.update(pc for change in modified for pc in change['guards'])
            lifter = HookLifter(iat_map=imports, lifted=entries, precise_sbb=True, precise_carry=True, guards=guards,
                               labels=function['pcs'], jump_targets=function['jump_targets'])
            body = lift_core.lift_function_linear(lifter, f'sub_{owner:08X}', modified[0]['original'], leaders, owner,
                                                 indirect_targets=function['jump_targets'])
            body = body.rsplit('\n}', 1)[0] + '\n    return;\n' + '\n'.join(snippet for change in modified for snippet in change['snippets']) + '\n}'
            replacements[f'{owner:08X}'] = body
        (OUT / f'{module}-functions.json').write_text(json.dumps(replacements), encoding='utf-8')
        (OUT / f'{module}-runtime.json').write_text(json.dumps(active_records), encoding='utf-8')
        print(f'{edition}: AOT functions: {len(replacements)}, active features: {len(active_records)}', flush=True)
    (OUT / 'catalog.json').write_text(json.dumps(catalog, ensure_ascii=False, indent=2), encoding='utf-8')
    emit_runtime()


def emit_runtime():
    import capstone
    import lift_core
    from tools.disasm.disasm32 import Instruction
    catalog = json.loads((OUT / 'catalog.json').read_text(encoding='utf-8'))
    catalog_metadata(catalog)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail = True
    for edition, module, filename in [('classic', 'classic', 'Stronghold Crusader.exe'), ('extreme', 'game', 'Stronghold_Crusader_Extreme.exe')]:
        records = {int(k): v for k, v in json.loads((OUT / f'{module}-runtime.json').read_text()).items()}
        binary = ROOT / 'generated/input' / filename
        pe = pefile.PE(str(binary)); image = pe.get_memory_mapped_image(); base = pe.OPTIONAL_HEADER.ImageBase
        info = lift_core.analyze_pe(binary); imports = lift_core.build_iat_map(info)
        entries = {int(a, 16) for a in re.findall(r'void sub_([0-9A-F]+)\(void\);', (ROOT / f'generated/{module}-x86/recomp_funcs.h').read_text())}
        header = ['/* Generated from the pinned UCP Lua scripts and checked original PE. */']
        header += ['static const char *ucp_names[] = {' + ','.join(json.dumps(f['id']) for f in catalog['features']) + '};']
        header += ['static const unsigned char ucp_available[] = {' + ','.join('1' if i in records else '0' for i in range(len(catalog['features']))) + '};']
        patches, initializers, regions, spans = [], [], [], {}
        extra = ['#define RECOMP_GENERATED_CODE', '#include "recomp_funcs.h"',
                 'extern unsigned char stronghold_ucp_enabled[];',
                 'extern void void_dispatch_add(uint32_t, void (*)(void));']
        registrations = []

        def array(data):
            name = f'ucp_blob_{len(header)}'
            header.append(f'static const unsigned char {name}[] = {{' + ','.join(str(b) for b in data) + '};')
            return name

        for index, record in records.items():
            patched = bytearray(image)
            ranges = []
            for address, _, after in record['writes']:
                if base <= address < base + len(image):
                    data = bytes.fromhex(after); patched[address-base:address-base+len(data)] = data
                    ranges.append((address, address+len(data)))
            spans[index] = merge_ranges(ranges + [tuple(v) for v in record.get('code_ranges', [])])
            for start, end in merge_ranges(ranges):
                before, after = image[start-base:end-base], bytes(patched[start-base:end-base])
                if before != after:
                    patches.append(f'{{{index},0x{start:X}u,{len(after)},{array(before)},{array(after)}}}')
            if record['allocations']:
                start = 0x50000000 + index * 0x100000
                end = max(a['address'] + len(bytes.fromhex(a['data'])) for a in record['allocations'])
                regions.append(f'{{{index},0x{start:X}u,{end-start}}}')
            code_segments = []
            for allocation in record['allocations']:
                data = bytes.fromhex(allocation['data']); address = allocation['address']
                if any(data):
                    initializers.append(f'{{{index},0x{address:X}u,{len(data)},NULL,{array(data)}}}')
                if allocation['code']:
                    code_segments.append((address, data))
            code = walk_code(md, code_segments, [address for address, _ in code_segments])
            if not code:
                continue
            pcs = {i.address for i in code}
            targets = {a['address'] for a in record['allocations'] if a['code']}
            for i in code:
                if i.is_call or i.mnemonic.startswith('mov'):
                    targets.update(op.imm for op in i.operands if op.type == capstone.x86.X86_OP_IMM and op.imm in pcs)
            for address, _, after in record['writes']:
                data = bytes.fromhex(after)
                targets.update(struct.unpack_from('<I', data, at)[0] for at in range(max(0, len(data)-3))
                               if struct.unpack_from('<I', data, at)[0] in pcs)
            for target in sorted(targets):
                lifter = lift_core.CheckedLifter(iat_map=imports, lifted=entries, precise_sbb=True, precise_carry=True)
                lifter._labels, lifter._jump_targets = pcs, pcs
                extra.append(lift_core.lift_function_linear(lifter, f'ucp_code_{target:08X}', sorted(code, key=lambda i:i.address), pcs, target))
                registrations.append(f'    if(stronghold_ucp_enabled[{index}]) void_dispatch_add(0x{target:X}u,ucp_code_{target:08X});')
        conflicts = []
        for first, a in spans.items():
            for second, b in spans.items():
                if first < second and any(x < v and u < y for x, y in a for u, v in b):
                    conflicts.append((first, second))
        catalog.setdefault('conflicts', {})[edition] = [[catalog['features'][a]['id'], catalog['features'][b]['id']] for a,b in conflicts]
        header.append('static const ucp_patch ucp_patches[] = {' + ','.join(patches) + '};')
        header.append('static const ucp_patch ucp_initializers[] = {' + ','.join(initializers) + '};')
        header.append('static const ucp_region ucp_regions[] = {' + ','.join(regions) + '};')
        header.append('static const unsigned char ucp_conflicts[][2] = {' + ','.join('{%d,%d}'%p for p in conflicts) + '};')
        destination = ROOT / f'generated/{module}-x86'
        (destination / 'ucp_data.h').write_text('\n'.join(header)+'\n')
        extra.append('void stronghold_ucp_register_code(void) {\n'+'\n'.join(registrations)+'\n}')
        (destination / 'ucp_extra.c').write_text('\n\n'.join(extra)+'\n')
    (OUT / 'catalog.json').write_text(json.dumps(catalog, ensure_ascii=False, indent=2), encoding='utf-8')


_replacements = {}
def apply(text, module):
    if module not in ('classic', 'game'):
        return text
    if module not in _replacements:
        _replacements[module] = json.loads((OUT / f'{module}-functions.json').read_text(encoding='utf-8'))
    changed = False
    def replace(match):
        nonlocal changed
        body = _replacements[module].get(match[1].decode())
        if body is None:
            return match[0]
        changed = True
        return body.encode()
    text = re.sub(rb'(?m)^void sub_([0-9A-F]{8})\(void\) \{.*?^\}', replace, text, flags=re.DOTALL)
    if changed:
        text = b'extern unsigned char stronghold_ucp_enabled[];\n' + text
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fetch', action='store_true')
    parser.add_argument('--runtime-only', action='store_true')
    parser.add_argument('--metadata-only', action='store_true')
    args = parser.parse_args()
    checkout(args.fetch)
    if args.metadata_only:
        update_catalog_metadata()
    elif args.runtime_only:
        emit_runtime()
    else:
        generate(extract())


if __name__ == '__main__':
    main()
