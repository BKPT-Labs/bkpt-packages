# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Preserve literal package and board routes without inferring populated wiring."""
import copy
import hashlib
import json
import re
import xml.etree.ElementTree as ET
import pdfplumber
from collections import Counter
from pathlib import Path

ROOT=Path(__file__).resolve().parent
PACK=Path('D:/Projects/BKPT-Data/firmware/sources/st/c5/pack-2.1.0')
DOCS=PACK.parent/'documents'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def build():
    base=json.loads((ROOT/'candidate.json').read_text(encoding='utf-8'))
    pins=json.loads((PACK/'pinout.json').read_text(encoding='utf-8'))
    board=json.loads((PACK/'netlist.json').read_text(encoding='utf-8'))
    bom=json.loads((PACK/'bom.json').read_text(encoding='utf-8'))
    parameters=json.loads((PACK/'parameters.json').read_text(encoding='utf-8'))
    assert board['pcb']=='MB2213' and board['revision']=='B02'
    assert bom['finished_good']==parameters['finished_good']=='NUC542RC$KR1'
    pdsc=ET.parse(PACK/'board.pdsc').getroot()
    board_nodes=[e for e in pdsc.iter() if e.tag.split('}')[-1]=='board' and e.get('name')=='NUCLEO-C542RC']
    assert len(board_nodes)==1 and board_nodes[0].get('revision')=='NUC542RC$KR1'
    mounted=[e for e in board_nodes[0].iter() if e.tag.split('}')[-1]=='mountedDevice']
    assert len(mounted)==1 and mounted[0].get('Dname')=='STM32C542RCT6'
    cuts=[e.attrib for e in board_nodes[0].iter() if e.tag.split('}')[-1]=='mountedDeviceCut']
    pcbs=[e for e in board_nodes[0].iter() if e.tag.split('}')[-1]=='pcb' and e.get('name')=='MB2213']
    assert len(pcbs)==1 and pcbs[0].get('revision')=='B02'
    mcu=[(i,p) for i,p in enumerate(bom['parts']) if p['pcb']=='MB2213' and p['reference']=='U1']
    assert len(mcu)==1 and mcu[0][1]['part_number']=='STM32C542RCT6'
    bonds={int(b['position']):b for b in pins['bonds']}
    assert len(bonds)==64 and set(bonds)==set(range(1,65))
    by_die={}
    for pad,bond in bonds.items():by_die.setdefault(bond['die_pad'],[]).append(pad)
    def normalize(name):
        match=re.match(r'^(P[A-H]\d+)(?:$|[-(])',name)
        return match[1] if match else name
    expected={p['pad']:p for p in base['package_pads']}
    # The base table was separately visually transcribed from the exact package drawing.
    for pad,bond in bonds.items():assert normalize(bond['die_pad'])==expected[pad]['signal']
    sources={name:{'path':str(PACK/name),'sha256':sha(PACK/name)}
             for name in ['pinout.json','netlist.json','bom.json','parameters.json','board.pdsc','device.pdsc']}
    routes=[]
    for index,signal in enumerate(pins['signals']):
        assert signal['die_pad'] in by_die
        assert len(by_die[signal['die_pad']])==1
        pad=by_die[signal['die_pad']][0]
        routes.append(dict(id=f'descriptor.signal.{index}',pin=normalize(signal['die_pad']),pad=pad,
            signal=signal['name'],instance=signal['instance'],function=signal['function'],
            source_pointer=f'/signals/{index}',source_record=signal,
            state='literal_descriptor_candidate',DS_AF_cell_crosscheck='not_complete',
            electrical_qualification='not_established',execution_permission='none'))
    nets=[]
    for index,net in enumerate(board['nets']):
        for member in net['net']:
            if member['reference']!='U1':continue
            pad=int(member['pin']);assert pad in expected
            nets.append(dict(pad=pad,pin=expected[pad]['signal'],net_id=net['netId'],label=net['netLabel'],
                members=net['net'],source_pointer=f'/nets/{index}',
                state='literal_same_net_members_only',bridge_or_component_conduction='not_inferred',
                actual_board_revision_and_population='unknown',execution_permission='none'))
    assert len(nets)==64 and {n['pad'] for n in nets}==set(range(1,65))
    analog={}
    for route in routes:
        if route['instance']=='ADC1' and route['function']['type']=='analog':
            analog.setdefault(route['signal'],[]).append({'pin':route['pin'],'pad':route['pad'],'route_id':route['id']})
    raw=json.loads((DOCS/'DS15135.pages.json').read_text(encoding='utf-8'))
    p40=next(p['text'] for p in raw['pages'] if p['page']==40)
    assert 'DAC2_OUT1' in p40 and any(r['pin']=='PA5' and r['signal']=='DAC1_OUT2' for r in routes)
    # Exact LQFP64 I/O-structure cells independently read from rendered Table12 pp39-45.
    cells='''PE2:FT PC13:FT_t PC14:FT PC15:FT PH0:FT_f PH1:FT_f
PC0:FT_a PC1:FT_a PC2:FT_a PC3:FT_a PA0:FT_a PA1:TT_a PA2:TT_a PA3:FT_a
PA4:TT_a PA5:TT_a PA6:TT_a PA7:FT_a PC4:FT_a PC5:FT_a PB0:TT_a PB1:TT_a PB2:FT_a
PB10:FT_f PB12:FT_f PB13:FT PB14:FT PB15:TT PC6:FT PC7:FT PC8:FT PC9:FT_f
PA8:FT_f PA9:FT PA10:FT PA11:FT_fu PA12:FT_fu PA13:FT PA14:FT PA15:FT
PC10:FT PC11:FT PC12:FT PD2:FT PB3:FT_f PB4:FT_f PB5:FT PB6:FT_f PB7:FT_f
PH2:FT PB8:FT_f PB9:FT_f'''
    ds_types=dict(cell.split(':') for cell in cells.split())
    assert len(ds_types)==52
    type_comparison=[]
    for pad,bond in sorted(bonds.items()):
        pin=normalize(bond['die_pad'])
        if pin not in ds_types:continue
        attr=pins['die_pads'][bond['die_pad']]['io_structure']
        descriptor=attr['type']+('_'+''.join(attr['options']) if attr.get('options') else '')
        page=next(page for high,page in [(14,39),(25,40),(35,41),(44,42),(54,43),(60,44),(64,45)] if pad<=high)
        type_comparison.append(dict(pin=pin,pad=pad,page=page,DS_cell=ds_types[pin],descriptor_cell=descriptor,
            state='agree_on_literal_label' if ds_types[pin]==descriptor else 'source_divergence',
            electrical_interpretation='not_qualified; labels and suffixes alone do not authorize a voltage or mode'))
    # Check the transcribed cells against independent extraction where rows are single-line.
    # PC15's name wraps to a separate line; its rendered row remains the independent evidence.
    for cell in type_comparison:
        if cell['pin']=='PC15':continue
        layout=next(p['layout_text'] for p in raw['pages'] if p['page']==cell['page'])
        pattern=rf"\b{cell['pad']}\s+{cell['pin']}(?=\s|[-(]).*?\bI/O\s+{cell['DS_cell']}\b"
        assert re.search(pattern,layout),cell
    divergent={c['pin'] for c in type_comparison if c['state']=='source_divergence'}
    for route in routes:
        route['unresolved_refs']=([f"IO_structure_comparison:{route['pin']}"] if route['pin'] in divergent else [])
        if route['pin']=='PA5' and route['signal']=='DAC1_OUT2':route['unresolved_refs'].append('conflicts:PA5_DAC_identity')
    # Bounded naming correspondence only; no prefix stripping for arbitrary names.
    aliases={name:'DEBUG_'+name for name in ['JTMS','SWDIO','JTCK','SWCLK','JTDI','JTDO','TRACESWO',
                                          'NJTRST','TRACED0','TRACED1','TRACED2','TRACED3','TRACECLK']}
    aliases.update({name:'RCC_'+name for name in ['MCO1','MCO2','AUDIOCLK']})
    af_cells=[]
    with pdfplumber.open(DOCS/'DS15135.pdf') as pdf:
        for page in [46,47]:
            tables=[t for t in pdf.pages[page-1].extract_tables() if len(t[0])==18]
            assert len(tables)==1
            table=tables[0];assert table[0][2:]==[f'AF{i}' for i in range(16)]
            for row_index,row in enumerate(table[2:],start=2):
                pin=row[1];assert pin in ds_types and len(row)==18
                for af,cell in enumerate(row[2:]):
                    assert isinstance(cell,str)
                    for raw_signal in cell.split('/'):
                        signal=re.sub(r'\s+','',raw_signal)
                        if signal=='-':continue
                        af_cells.append(dict(pin=pin,af=f'AF{af}',DS_signal=signal,descriptor_name=aliases.get(signal,signal),
                            page=page,table=13,extracted_row=row_index,raw_cell=cell))
    assert len(af_cells)==385 and {c['pin'] for c in af_cells}==set(ds_types)
    by_cell={(c['pin'],c['af'],c['descriptor_name']):c for c in af_cells}
    assert len(by_cell)==385
    descriptor_af=set()
    for route in routes:
        if route['function']['type']!='alternate':continue
        key=(route['pin'],route['function']['id'],route['signal']);assert key in by_cell
        descriptor_af.add(key)
        route['DS_AF_cell_crosscheck']='literal_pin_AF_name_correspondence_only'
        route['DS_AF_evidence']=by_cell[key]
    ds_only=[dict(cell,state='DS_literal_missing_from_descriptor',implementation='not_authored',execution_permission='none')
             for key,cell in by_cell.items() if key not in descriptor_af]
    assert len(ds_only)==52 and all(c['af']=='AF15' and c['DS_signal']=='EVENTOUT' for c in ds_only)
    sources['DS15135']={'path':str(DOCS/'DS15135.pdf'),'sha256':raw['document_sha256'],
        'extraction_path':str(DOCS/'DS15135.pages.json'),'extraction_sha256':sha(DOCS/'DS15135.pages.json')}
    return dict(schema='bkpt.c5-package-board-routes/1',publication='unpublished',target=base['target'],
        base_candidate_sha256=sha(ROOT/'candidate.json'),sources=sources,
        scope='Literal descriptor inventory and direct U1 net memberships; not a functional board schematic or electrical authorization.',
        descriptor_counts=dict(Counter(r['function']['type'] for r in routes)),routes=routes,
        raw_descriptor_die_pad_attributes=pins['die_pads'],io_structure_descriptions=pins['io_structure_type_description'],
        pad_qualifications={c['pin']:{'IO_comparison_ref':f"IO_structure_comparison:{c['pin']}",
            'state':c['state'],'policy':'No voltage tolerance or electrical permission inferred from raw descriptor attributes; reconcile all TT/FT and suffix differences first.'} for c in type_comparison},
        io_options_descriptions=pins['io_structure_options_description'],
        descriptor_naming_qualification='COMP input selector suffixes and PWR_WKUP prefixes are literal descriptor names; equivalence to shorter DS names and shared selector exclusivity remain unreviewed.',
        IO_structure_comparison=type_comparison,
        AF_comparison={'extractor':'pdfplumber','extractor_version':pdfplumber.__version__,
            'source_pages':[46,47],'naming_correspondence':aliases,'descriptor_AF_matches':333,
            'DS_signal_occurrences':385,'DS_only_signals':ds_only,
            'qualification':'Literal table-cell correspondence; alias list is bounded author normalization, not independent functional/SDK qualification.'},
        IO_structure_visual_evidence=[{'page':p,'path':str(ROOT/('package-conflict-check-040.png' if p==40 else f'package-electrical-check-{p:03d}.png')),
            'sha256':sha(ROOT/('package-conflict-check-040.png' if p==40 else f'package-electrical-check-{p:03d}.png')),
            'scope':'LQFP64 pin, I/O-structure label and associated row inspected; electrical limits not validated'} for p in range(39,46)],
        board={'name':'NUCLEO-C542RC','finished_good':bom['finished_good'],'pcb':'MB2213','revision':'B02',
            'identity_evidence':{'BOM_pointer':f'/parts/{mcu[0][0]}','BOM_record':mcu[0][1],
                'PDSC_mounted_device':mounted[0].attrib,'PDSC_board':board_nodes[0].attrib,'PDSC_pcb':pcbs[0].attrib},
            'supply_descriptor':parameters['parameters']['board'],'actual_supply':'not_measured',
            'descriptor_mounted_device_cuts':cuts,'actual_silicon_revision':'unknown; descriptor cut is not a physical observation',
            'U1_net_memberships':sorted(nets,key=lambda n:n['pad']),
            'missing_for_operational_connectivity':['component_pin_functions','bridge_and_jumper_state','population_and_variant',
                'actual_board_revision','external_connections','electrical_limits_and_power_state']},
        adc_signal_alias_groups=analog,
        alias_qualification='Repeated signal name is one descriptor identity, not additional ADC channels; analog switch/register semantics remain unreviewed.',
        conflicts=[
            {'id':'NbIOs_count','descriptor':pins['characteristics']['NbIOs'],
             'bonded_GPIO_records':sum(r['function']['type']=='user_gpio' for r in routes),'state':'unresolved_source_discrepancy'},
            {'id':'PA1_IO_structure','DS15135_p40':'TT_a','descriptor':pins['die_pads']['PA1']['io_structure'],
             'state':'conflicting','policy':'Do not infer 5V tolerance from descriptor FT; applies to every TT/FT divergence in IO_structure_comparison.'},
            {'id':'PA5_DAC_identity','DS15135_p40':'DAC2_OUT1','descriptor':'DAC1_OUT2',
             'state':'conflicting','policy':'No DAC2 instance inferred; route unavailable for executable promotion pending resolution.'}],
        evidence={'DS15135:p40':{'page':40,'text':p40,'sha256':hashlib.sha256(p40.encode()).hexdigest(),
            'visual_path':str(ROOT/'package-conflict-check-040.png'),
            'visual_sha256':sha(ROOT/'package-conflict-check-040.png'),
            'visual_check':'PA1 TT_a and PA5 DAC2_OUT1 cells inspected'}},
        full_package_semantic_coverage=False,board_operational_qualification='not_established',execution_permission='none')


def verify(bundle):
    assert bundle==build()
    assert bundle['descriptor_counts']=={'user_gpio':52,'analog':39,'alternate':333,'additional':16}
    assert len(bundle['routes'])==440 and len(bundle['adc_signal_alias_groups'])==12
    assert bundle['AF_comparison']['descriptor_AF_matches']==333 and len(bundle['AF_comparison']['DS_only_signals'])==52
    assert len(bundle['IO_structure_comparison'])==52
    assert sum(r['state']=='source_divergence' for r in bundle['IO_structure_comparison'])==11
    for record in bundle['IO_structure_visual_evidence']:assert sha(record['path'])==record['sha256']
    assert {r['pin'] for r in bundle['routes'] if r['function']['type']=='user_gpio'}=={
        p['signal'] for p in json.loads((ROOT/'candidate.json').read_text(encoding='utf-8'))['package_pads']
        if re.fullmatch(r'P[A-H]\d+',p['signal'])}
    assert len({r['id'] for r in bundle['routes']})==440
    for source in bundle['sources'].values():assert sha(source['path'])==source['sha256']
    assert all(r['execution_permission']=='none' for r in bundle['routes'])
    assert bundle['full_package_semantic_coverage'] is False and bundle['execution_permission']=='none'


if __name__=='__main__':
    bundle=build();verify(bundle)
    cases=[]
    for name,change in [
        ('dropped_route',lambda b:b['routes'].pop()),
        ('unbonded_pin',lambda b:b['routes'][0].update(pin='PB11')),
        ('wrong_AF',lambda b:next(r for r in b['routes'] if r['function']['type']=='alternate')['function'].update(id='AF99')),
        ('wrong_board',lambda b:b['board'].update(revision='B01')),
        ('wrong_U1_net',lambda b:b['board']['U1_net_memberships'][0].update(net_id=999)),
        ('silenced_conflicts',lambda b:b.update(conflicts=[])),
        ('silenced_IO_divergence',lambda b:b['IO_structure_comparison'][1].update(state='resolved')),
        ('dropped_DS_only_EVENTOUT',lambda b:b['AF_comparison']['DS_only_signals'].pop()),
        ('electrical_promotion',lambda b:b.update(full_package_semantic_coverage=True))]:
        altered=copy.deepcopy(bundle);change(altered)
        try:verify(altered)
        except (AssertionError,KeyError):cases.append({'id':name,'result':'rejected'})
        else:raise AssertionError(name)
    (ROOT/'package-routes.json').write_text(json.dumps(bundle,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
    (ROOT/'package-routes-acceptance.json').write_text(json.dumps(dict(bundle_sha256=sha(ROOT/'package-routes.json'),
        author_sha256=sha(__file__),negative_fixtures=cases,route_count=440,U1_pad_count=64,
        qualification='Literal source reconciliation; independent AF-cell and electrical validation incomplete.'),indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'routes':440,'U1_pads':64,'negative_fixtures':len(cases),'sha256':sha(ROOT/'package-routes.json')}))
