# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Headless source-integrity and negative allocation fixtures for candidate data."""
import copy
import hashlib
import json
import re
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent
TARGET = dict(device='STM32C542RCT6',device_group='STM32C542xx',package='LQFP64',
              silicon_revision='unknown',sdk='stm32c5-hal2-2.1.0')
FUNCTIONS = {'PH0':['OSC_IN'],'PH1':['OSC_OUT'],'PH2':['BOOT0'], 'PC14':['OSC32_IN'],
             'PC15':['OSC32_OUT'],'PA13':['JTMS','SWDIO'],'PA14':['JTCK','SWCLK'],
             'PA15':['JTDI'],'PB3':['JTDO','TRACESWO'],'PB4':['NJTRST'],
             'PC13':['LSE_crystal_interaction_requires_revision_context']}
VISUALS = {'ds-table-check.png':('DS15135',5),'ds-pin-check.png':('DS15135',37),
           'dma-instance-check.png':('RM0522',383),'uart-modes-check.png':('DS15135',28),
           'dma-conflict-check.png':('DS15135',13)}
FAMILY = {'XSPI','DLYB','CCB','SAES','PKA','ETH','TIM3','TIM4','TIM5','TIM16','TIM17'}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def integrity(bundle):
    assert bundle['publication'] == 'unpublished'
    assert bundle['target']['device'] == 'STM32C542RCT6'
    assert bundle['target']['package'] == 'LQFP64'
    assert bundle['target'] == TARGET
    assert bundle['closure']['complete_device'] is False
    assert bundle['closure']['complete_domains'] == 0
    states = dict(review='candidate', implementation='not_authored_in_this_bundle',
                  build='not_tested', hardware='not_tested', closure='open')
    for source in bundle['sources'].values():
        assert sha(Path(source['path']).read_bytes()) == source['sha256']
        if 'extraction_path' in source:
            assert sha(Path(source['extraction_path']).read_bytes()) == source['extraction_sha256']
    pages_by_source = {}
    for key, source in bundle['sources'].items():
        if 'extraction_path' in source:
            raw = json.loads(Path(source['extraction_path']).read_text(encoding='utf-8'))
            assert raw['document_sha256'] == source['sha256']
            pages_by_source[key] = {p['page']:p for p in raw['pages']}
    for key, excerpt in bundle['excerpts'].items():
        assert key == f"{excerpt['source']}:p{excerpt['locator']['page']}"
        checked = (excerpt['source'],excerpt['locator']['page']) in VISUALS.values()
        assert excerpt['visual_check'] == ('author_checked' if checked else 'not_checked')
        assert sha(excerpt['text'].encode()) == excerpt['text_sha256']
        original = pages_by_source[excerpt['source']][excerpt['locator']['page']]
        assert original['text'] == excerpt['text']
    ids = [c['id'] for c in bundle['claims']]
    assert len(ids) == len(set(ids))
    for claim in bundle['claims']:
        assert claim['evidence'] and all(ref in bundle['excerpts'] for ref in claim['evidence'])
        assert claim['applicability'] == bundle['target']
        assert claim['states']['review'] == 'candidate'
        assert claim['states']['hardware'] == 'not_tested'
        assert claim['states']['build'] == 'not_tested'
        assert claim['states']['closure'] == 'open'
        assert claim['states'] == states
        assert claim['availability'] in ('source_known','conflicting','unknown')
        assert claim['errata_coverage'] == 'incomplete'
        assert claim['limitations']
    pins = bundle['package_pads']
    assert sorted(p['pad'] for p in pins) == list(range(1,65))
    assert len({p['signal'] for p in pins if re.fullmatch(r'P[A-H]\d+', p['signal'])}) == 52
    assert not any(p['signal'] == 'PB11' for p in pins)
    assert all(p['review'] == 'candidate' and p['board_access'] == 'unknown' for p in pins)
    for pin in pins:
        assert pin['device_function_constraints'] == FUNCTIONS.get(pin['signal'],[])
        assert pin['reservation_state'] == ('requires_context' if pin['device_function_constraints'] else 'not_assessed')
    transcription = json.loads((ROOT/'pin-transcription.json').read_text(encoding='utf-8'))
    assert transcription['source_sha256'] == bundle['sources']['DS15135']['sha256']
    assert [(p['pad'],p['signal']) for p in pins] == [(p['pad'],p['signal']) for p in transcription['pads']]
    header_lines = Path(bundle['sources']['header']['path']).read_text(encoding='utf-8').splitlines()
    expected_irqs = []
    for line,text in enumerate(header_lines,1):
        match = re.match(r'\s*(\w+_IRQn)\s*=\s*(-?\d+),?\s*(.*)', text)
        if match:
            expected_irqs.append(dict(symbol=match[1],number=int(match[2]),comment=match[3],
                                      source='header',line=line,kind='SDK_declaration',review='candidate'))
    assert bundle['irq_declarations'] == expected_irqs
    for excerpt in bundle['sdk_excerpts']:
        text = '\n'.join(header_lines[excerpt['start_line']-1:excerpt['end_line']])
        assert text == excerpt['text'] and sha(text.encode()) == excerpt['text_sha256']
    for visual in bundle['visual_evidence']:
        assert sha(Path(visual['path']).read_bytes()) == visual['sha256']
        assert visual['document_sha256'] == bundle['sources'][visual['document']]['sha256']
        assert VISUALS[Path(visual['path']).name] == (visual['document'],visual['page'])
    assert {Path(v['path']).name for v in bundle['visual_evidence']} == set(VISUALS)
    svd = ET.parse(bundle['sources']['svd']['path']).getroot()
    svd_names = [p.findtext('name') for p in svd.findall('./peripherals/peripheral')]
    assert [i['id'] for i in bundle['inventory']] == svd_names + ['CORDIC']
    assert all(i['states'] == states for i in bundle['inventory'])
    subjects = {i['id'] for i in bundle['inventory']} | set(bundle['subject_groups']) | set(bundle['source_aliases']) | {d['id'] for d in bundle['work_domains']}
    assert all(c['subject'] in subjects for c in bundle['claims'])
    assert all(s in subjects for group in bundle['subject_groups'].values() for s in group)
    assert subjects.isdisjoint(FAMILY)
    assert {i['id'] for i in bundle['family_only_investigation']} == FAMILY
    assert all(i['availability']=='unknown' for i in bundle['family_only_investigation'])
    assert set(bundle['source_aliases']) == {'ADCC12','LPUART1','COMP1','COMP2'}
    assert {i['id'] for i in bundle['work_domains']} == {'CPU/FPU/DSP','NVIC','SysTick','MPU','SRAM/bus_matrix',
        'boot/system_memory','option_bytes/lifecycle','electronic_signature','package/electrical',
        'board_routing','interconnect','silicon_errata'}
    assert {i['id'] for i in bundle['conflicts']} == {'cordic_svd','dma_channel_table','exti_event_counts','nvic_count','fdcan_count'}
    for name in ['LPDMA1','LPDMA2']:
        assert next(c['value'] for c in bundle['claims'] if c['id']==name+'.channels') == [0,1,2,3]
    assert sha(Path(bundle['baseline']['path']).read_bytes()) == bundle['baseline']['sha256']
    assert not any(c['subject'] == 'ADC2' for c in bundle['claims'])


def allocate(bundle, requests):
    """Check ONLY candidate identity constraints. Never return executable/validated."""
    pads = {p['signal']: p['pad'] for p in bundle['package_pads'] if re.fullmatch(r'P[A-H]\d+', p['signal'])}
    used = {}
    mux_selection = {}
    reservations = {p['signal'] for p in bundle['package_pads'] if p['device_function_constraints']}
    for index, request in enumerate(requests):
        if request.get('target') != 'STM32C542RCT6/LQFP64':
            return 'unknown_applicability'
        kind = request.get('kind')
        allowed = {'kind','target','owner'} | ({'pin','mode'} if kind=='gpio' else {'pin'} if kind=='exti' else {'controller','channel','endpoint_pair'} if kind=='dma' else set())
        if set(request)-allowed:
            return 'unknown_request_field'
        if 'owner' not in request or not isinstance(request['owner'],str) or not request['owner']:
            return 'unknown_owner'
        resources = []
        if kind in ('gpio', 'exti'):
            pin = request.get('pin')
            if pin not in pads:
                return 'outside_package_pin_domain'
            if pin in reservations:
                return 'unknown_device_function_reservation'
            if kind == 'gpio' and request.get('mode') != 'input':
                return 'unknown_gpio_mode_contract'
            resources.append(('pad', pads[pin]))
            if kind == 'exti':
                line = int(re.fullmatch(r'P[A-H](\d+)', pin)[1])
                if line in mux_selection and mux_selection[line] != pin:
                    return 'resource_conflict'
                mux_selection[line] = pin
                resources.append(('EXTI', line))
        elif kind == 'dma':
            controller, channel = request.get('controller'), request.get('channel')
            if controller not in ('LPDMA1', 'LPDMA2') or type(channel) is not int:
                return 'unknown_applicability'
            channels = next(c['value'] for c in bundle['claims'] if c['id']==controller+'.channels')
            if channel not in channels:
                return 'outside_declared_candidate_domain'
            resources.append((controller, channel))
            if 'endpoint_pair' in request:
                return 'unknown_endpoint_pair'
        else:
            return 'unknown_contract'
        owner = request['owner']
        if not isinstance(owner,str) or not owner:
            return 'unknown_owner'
        if any(resource in used and used[resource] != owner for resource in resources):
            return 'resource_conflict'
        for resource in resources:
            # Same-owner sharing is permitted only for GPIO/EXTI identity checks.
            if resource in used and kind == 'dma':
                return 'resource_conflict'
            used[resource] = owner
    return 'identity_only_context_unknown'


def run():
    bundle = json.loads((ROOT/'candidate.json').read_text(encoding='utf-8'))
    manifest = json.loads((ROOT/'manifest.json').read_text(encoding='utf-8'))
    assert sha((ROOT/'candidate.json').read_bytes()) == manifest['candidate_sha256']
    assert sha((ROOT/'author.py').read_bytes()) == manifest['author_sha256']
    assert sha((ROOT/'contracts.json').read_bytes()) == manifest['contracts_sha256']
    assert sha((ROOT/'pin-transcription.json').read_bytes()) == manifest['transcription_sha256']
    contracts = json.loads((ROOT/'contracts.json').read_text(encoding='utf-8'))
    assert contracts['publication'] == 'unpublished'
    assert contracts['schema'] == 'bkpt.coverage-contract-proposal/1'
    assert contracts['target'] == 'STM32C542RCT6/LQFP64'
    assert all(ref in bundle['excerpts'] for rule in contracts['requirements'] for ref in rule['evidence'])
    dma_contract=next(c for c in contracts['requirements'] if c['id']=='dma-channel')
    for name in ['LPDMA1','LPDMA2']:
        assert dma_contract['candidate_domain'][name] == next(c['value'] for c in bundle['claims'] if c['id']==name+'.channels')
    assert manifest['counts'] == dict(svd_nodes=51,additional_source_known_blocks=1,claims=len(bundle['claims']),
                                     package_pads=64,irq_declarations=len(bundle['irq_declarations']),complete_domains=0)
    integrity(bundle)
    results = []
    request_sequence = 0
    def req(kind, **values):
        nonlocal request_sequence
        request_sequence += 1
        values.setdefault('owner',f'fixture-owner-{request_sequence}')
        if kind=='gpio': values.setdefault('mode','input')
        return dict(kind=kind, target='STM32C542RCT6/LQFP64', **values)
    fixtures = [
        ('two_independent_pads', [req('gpio',pin='PA4'),req('gpio',pin='PA5')], 'identity_only_context_unknown'),
        ('pin_double_use', [req('gpio',pin='PA4'),req('exti',pin='PA4')], 'resource_conflict'),
        ('same_owner_gpio_exti', [req('gpio',pin='PA4',owner='sensor'),req('exti',pin='PA4',owner='sensor')], 'identity_only_context_unknown'),
        ('debug_pad', [req('gpio',pin='PA13')], 'unknown_device_function_reservation'),
        ('boot_pad', [req('gpio',pin='PH2')], 'unknown_device_function_reservation'),
        ('oscillator_pad', [req('gpio',pin='PC14')], 'unknown_device_function_reservation'),
        ('same_exti_line_different_ports', [req('exti',pin='PA6'),req('exti',pin='PB6')], 'resource_conflict'),
        ('same_owner_cannot_select_two_ports', [req('exti',pin='PA6',owner='x'),req('exti',pin='PB6',owner='x')], 'resource_conflict'),
        ('different_exti_lines', [req('exti',pin='PA4'),req('exti',pin='PB5')], 'identity_only_context_unknown'),
        ('unbonded_pin', [req('gpio',pin='PB11')], 'outside_package_pin_domain'),
        ('supply_is_not_gpio', [req('gpio',pin='VDD')], 'outside_package_pin_domain'),
        ('dma_channel_double_use', [req('dma',controller='LPDMA1',channel=0)]*2, 'resource_conflict'),
        ('distinct_controllers', [req('dma',controller='LPDMA1',channel=0),req('dma',controller='LPDMA2',channel=0)], 'identity_only_context_unknown'),
        ('family_channel_leak', [req('dma',controller='LPDMA1',channel=7)], 'outside_declared_candidate_domain'),
        ('conflicting_channel4', [req('dma',controller='LPDMA1',channel=4)], 'outside_declared_candidate_domain'),
        ('boolean_is_not_channel_index', [req('dma',controller='LPDMA1',channel=True)], 'unknown_applicability'),
        ('direction_not_endpoint_proof', [req('dma',controller='LPDMA1',channel=0,endpoint_pair=['SPI1','SPI2'])], 'unknown_endpoint_pair'),
        ('empty_endpoint_pair', [req('dma',controller='LPDMA1',channel=0,endpoint_pair=[])], 'unknown_endpoint_pair'),
        ('null_endpoint_pair', [req('dma',controller='LPDMA1',channel=0,endpoint_pair=None)], 'unknown_endpoint_pair'),
        ('string_endpoint_pair', [req('dma',controller='LPDMA1',channel=0,endpoint_pair='')], 'unknown_endpoint_pair'),
        ('unknown_request_selector', [req('dma',controller='LPDMA1',channel=0,request='xspi1_dma')], 'unknown_request_field'),
        ('unknown_trigger_selector', [req('dma',controller='LPDMA1',channel=0,trigger='lpdma1_ch4_tc')], 'unknown_request_field'),
        ('unmodeled_early_termination', [req('dma',controller='LPDMA1',channel=1,PFREQ=1)], 'unknown_request_field'),
        ('output_not_input_composition', [req('gpio',pin='PA4',mode='output',owner='x'),req('exti',pin='PA4',owner='x')], 'unknown_gpio_mode_contract'),
        ('analog_not_input_composition', [req('gpio',pin='PA4',mode='analog',owner='x'),req('exti',pin='PA4',owner='x')], 'unknown_gpio_mode_contract'),
        ('pc13_lse_context', [req('gpio',pin='PC13')], 'unknown_device_function_reservation'),
        ('no_implicit_owner_to_spoof', [dict(kind='gpio',pin='PA4',mode='input',target='STM32C542RCT6/LQFP64')], 'unknown_owner'),
        ('other_package', [dict(kind='gpio',pin='PA4',target='STM32C542CCT6/LQFP48')], 'unknown_applicability'),
        ('unknown_mode_contract', [req('cordic')], 'unknown_contract')]
    for name, requests, expected in fixtures:
        actual = allocate(bundle, requests)
        assert actual == expected, (name, actual, expected)
        results.append(dict(id=name, expected=expected, observed=actual, passed=True))
    for name, mutate in [
        ('forged_excerpt', lambda b: b['excerpts']['DS15135:p5'].update(text='fabricated')),
        ('missing_evidence', lambda b: b['claims'][0].update(evidence=[])),
        ('silent_review_promotion', lambda b: b['claims'][0]['states'].update(review='reviewed')),
        ('invented_pad', lambda b: b['package_pads'][0].update(signal='PE3')),
        ('invented_irq', lambda b: b['irq_declarations'][0].update(number=999)),
        ('inventory_review_promotion', lambda b: b['inventory'][0]['states'].update(review='reviewed')),
        ('implementation_promotion', lambda b: b['claims'][0]['states'].update(implementation='hardware_qualified')),
        ('availability_promotion', lambda b: b['claims'][0].update(availability='hardware_supported')),
        ('locator_substitution', lambda b: b['excerpts'].__setitem__('DS15135:p5',copy.deepcopy(b['excerpts']['DS15135:p13']))),
        ('family_domain_expansion', lambda b: next(c for c in b['claims'] if c['id']=='LPDMA1.channels').update(value=list(range(8)))),
        ('drop_debug_reservation', lambda b: next(p for p in b['package_pads'] if p['signal']=='PA14').update(device_function_constraints=[])),
        ('drop_conflict', lambda b: b.update(conflicts=[])),
        ('drop_family_scope', lambda b: b.update(family_only_investigation=[])),
        ('drop_system_domains', lambda b: b.update(work_domains=[])),
        ('false_complete_domains', lambda b: b['closure'].update(complete_domains=1)),
        ('false_visual_check', lambda b: b['excerpts']['RM0522:p342'].update(visual_check='author_checked')),
        ('visual_page_substitution', lambda b: b['visual_evidence'][0].update(page=6)),
        ('family_alias_injection', lambda b: b['source_aliases'].update(TIM3={'svd_node':'TIM2'})),
        ('target_and_claim_widening', lambda b: (b['target'].update(silicon_revision='all'),[c['applicability'].update(silicon_revision='all') for c in b['claims']])),
        ('silicon_revision_widening', lambda b: b['claims'][0]['applicability'].update(silicon_revision='all'))]:
        changed = copy.deepcopy(bundle)
        mutate(changed)
        try:
            integrity(changed)
        except (AssertionError,KeyError):
            results.append(dict(id=name, passed=True, observed='rejected'))
        else:
            raise AssertionError(f'{name}: invalid candidate accepted')
    report = dict(candidate_sha256=manifest['candidate_sha256'], manifest_sha256=sha((ROOT/'manifest.json').read_bytes()),
                  validator_sha256=sha(Path(__file__).read_bytes()),
                  fixtures=results, passed=len(results), failed=0,
                  qualification='Host evidence/identity fixtures only; no firmware build, device behavior or full semantic review.',
                  complete_domains=0)
    (ROOT/'acceptance.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k != 'fixtures'}, indent=2))


if __name__ == '__main__':
    run()
