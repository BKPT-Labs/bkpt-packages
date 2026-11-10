# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Pinned ES0676 inventory with explicit applicability, never execution approval."""
import copy
import hashlib
import json
import re
from pathlib import Path

ROOT=Path(__file__).resolve().parent
DOCS=Path('D:/Projects/BKPT-Data/firmware/sources/st/c5/documents')

# Independently transcribed revision cells from visually inspected Table 3, pages 2-3.
STATUS={'2.1.1':'NN','2.2.1':'AA','2.2.2':'NN','2.2.3':'NN','2.3.1':'AA','2.3.2':'AA',
        '2.4.1':'N-','2.5.1':'NN','2.6.1':'AA','2.6.2':'AA','2.6.3':'NN','2.7.1':'NN',
        '2.7.2':'AA','2.7.3':'NN','2.8.1':'PP','2.8.2':'AA','2.9.1':'AA','2.9.2':'NN',
        '2.10.1':'PP','2.11.1':'NN','2.12.1':'AA','2.12.2':'AA','2.13.1':'AA'}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def build():
    base=json.loads((ROOT/'candidate.json').read_text(encoding='utf-8'))
    doc=json.loads((DOCS/'ES0676.pages.json').read_text(encoding='utf-8'))
    pages={p['page']:p['text'] for p in doc['pages']}
    entries=[]
    def entry(section,domain,detail_pages,condition,response,qualification='conditional_on_application_context'):
        entries.append(dict(section=section,domain=domain,detail_pages=detail_pages,
            revision_status=dict(zip(['Z','Y'],STATUS[section])),condition=condition,response=response,
            exact_device_applicability=qualification,review='candidate',implementation='not_audited',
            build='not_tested',hardware='not_observed',execution_permission='none'))
    entry('2.1.1','Core',[4],
        'MPU enabled; unaligned load/store hits exactly one device-memory region whose permissions prohibit the access.',
        'No workaround. Do not depend on UsageFault taking priority over the access-permission MemManage fault.')
    entry('2.2.1','RCC',[4], 'LSE analog bypass: LSEBYP=1 and LSEEXT=0.',
        'Use digital bypass instead: LSEBYP=1 and LSEEXT=1.')
    entry('2.2.2','RCC/GPIOC',[4,5], 'LQFP package, LSE crystal operating, PC13 toggling as input or output.',
        'No workaround in table; avoid PC13 toggling while using the crystal. Bypass is unaffected by this entry only; analog bypass has its own erratum.')
    entry('2.2.3','RCC',[5], 'LSEDRV=00 low drive.',
        'Mode forbidden; no workaround. Other drive modes still require electrical qualification.')
    entry('2.3.1','ADC1/ADCC12',[5],
        'Family description requires ADC1 and ADC2 simultaneous regular mode DUAL=0x06, with injected stop coinciding with injected trigger.',
        'Family alternatives: prevent the listed events coinciding; stop regular before injected conversions; stop injected trigger source first; or use DUAL=0x01/0x02. These are not admitted C542 modes.',
        'unresolved_family_applicability_exact_C542_has_one_ADC_do_not_infer_ADC2_or_dual_mode')
    entry('2.3.2','ADC1',[5], 'SMPTRIG set, regular conversion starts then stops, SMPTRIG cleared, then injected conversion attempted.',
        'Disable ADC, clear SMPTRIG, then re-enable ADC before injected conversions.')
    entry('2.4.1','COMP1/COMP2',[6], 'Both comparators enabled simultaneously.',
        'Rev Z: use individually; no workaround for concurrent operation. Rev Y: this specific limitation is marked not applicable; other constraints still apply.')
    entry('2.5.1','RNG',[6], 'Supply, temperature and process combinations cause insufficient analog entropy and continuous seed errors.',
        'Table status N; body requires standard RNG error-management procedure for detection and recovery. No extra application workaround is specified; do not accept seed-error output.')
    entry('2.6.1','LPTIM1',[6], 'Clearing ENABLE near an interrupt can leave wake signal active and prevent entry to any Stop mode.',
        'Disable through assert/deassert of the RCC peripheral reset, rather than clearing ENABLE. Reset impacts the entire owned peripheral configuration.')
    entry('2.6.2','LPTIM1',[6,7], 'One-shot, APB slower than kernel, odd programmed REP value (not parity of effective REP+1).',
        'Require both ARR and CMP >= KER_CLK/(2*APB_CLK). Use integer-safe comparison; source example REP=3 has four effective repetitions.')
    entry('2.6.3','LPTIM1',[7], 'Modification of an interrupt bit in LPTIM_DIER.',
        'Corresponding ISR flag is cleared by hardware; no workaround. Changing enables must account for pending-event loss.')
    entry('2.7.1','RTC',[7], 'Core halted in debug; MASKSS nonzero; subsecond alarm met just before halt/printf, even with RTC freeze configured.',
        'No workaround. ALRAF/ALRBF may repeatedly reassert; clearing while halted can be ineffective.')
    entry('2.7.2','RTC',[7,8], 'Calendar read while BYPSHAD=0; shadow copy can transiently be incorrect.',
        'Set BYPSHAD=1; read calendar twice and repeat until consecutive readings agree; follow RM bypass calendar-read protocol.')
    entry('2.7.3','RTC/TAMP/PWR',[8], 'Write 1 to RTC_SCR, TAMP_SCR or TAMP_CR2.BKERASE while backup-domain writes are protected in PWR.',
        'Follow correct write-access sequence before touching these bits. No recovery workaround; system reset is required after lockup. PWR bit name is missing in both extraction and rendered PDF, so exact field binding remains unresolved.')
    entry('2.8.1','I2C1',[8], 'Transmitter minimum data setup time is shorter than one I2C kernel-clock period.',
        'Increase kernel frequency or transmitter setup time. At standard minima 250/100/50 ns, require respectively >=4/10/20 MHz. Partial workaround status P is preserved.')
    entry('2.8.2','I2C1',[9], 'Controller-mode spurious BERR and optional error interrupt; transfer itself continues.',
        'Clear BERR in software and continue normal handling. Do not infer this action for target mode or unrelated faults; source register spelling needs SDK reconciliation.')
    entry('2.9.1','USART capability intersection',[9], 'Disable autobaud by clearing ABREN after detection while reception continues.',
        'Do not clear ABREN. Apply only to exact instances with autobaud capability; inherited register layout is insufficient.')
    entry('2.9.2','USART capability intersection',[9], 'ONEBIT=1 and noise detected on START bit.',
        'NE may set despite single-sample mode; no workaround. Bind to exact supported instances before implementation.')
    entry('2.10.1','LPUART1',[9,10], 'Noninteger kernel-frequency/baud ratio between three and four; example 32768/9600.',
        'Increase transmitter ratio by raising kernel frequency or lowering baud; alternatively use higher-frequency receiver oversampling if supported. Partial workaround; no proven universal ratio threshold supplied.')
    entry('2.11.1','SPI capability intersection',[10], 'Target/slave with RDY output enabled via RDIOM; serial clock exceeds 2*APB for 8..15 bits, 6*APB for 16..23, or 14*APB for 24..32.',
        'No workaround for affected configuration. Do not infer safety or applicability for unlisted widths or instances without RDY support.')
    entry('2.12.1','FDCAN1/FDCAN2',[10], 'EFBI enabled and integration-phase end coincides with falling RX edge.',
        'Disable edge filtering or rely on retransmission. Source separately says standard-frame reception is unaffected; preserve that qualifier pending exact protocol interpretation.')
    entry('2.12.2','FDCAN1/FDCAN2',[10,11], 'Dedicated Tx buffer plus Tx FIFO (TFQM=0), with buffer messages having higher internal CAN priority.',
        'Choose one documented strategy: only one FIFO transmission pending at once; exclusively use FIFO; or schedule dedicated buffers. Do not treat example buffer numbers as guaranteed exact-device allocation.')
    entry('2.13.1','USB',[11], 'OUT-transfer CTR arrives before final USB SRAM accesses finish.',
        'Delay before SRAM access: 800 ns Full Speed, 6.4 us Low Speed. Retain both family values; admit only exact-device supported speed modes.')
    used={1,2,3,*[p for e in entries for p in e['detail_pages']]}
    return dict(schema='bkpt.c5-errata-inventory/1',publication='unpublished',target=base['target'],
        base_candidate_sha256=sha(ROOT/'candidate.json'),source={'pdf':str(DOCS/'ES0676.pdf'),
        'sha256':doc['document_sha256'],'extraction':str(DOCS/'ES0676.pages.json'),
        'extraction_sha256':sha(DOCS/'ES0676.pages.json'),'revision':2},
        evidence={str(p):{'page':p,'text':pages[p],'text_sha256':hashlib.sha256(pages[p].encode()).hexdigest()} for p in sorted(used)},
        visual_table_checks=[dict(page=p,path=str(ROOT/f'errata-table-check-{p:02d}.png'),
            sha256=sha(ROOT/f'errata-table-check-{p:02d}.png'),status='visually_checked_revision_columns') for p in [2,3]],
        visual_source_defects=[dict(page=8,path=str(ROOT/'errata-protection-check-08.png'),
            sha256=sha(ROOT/'errata-protection-check-08.png'),finding='PWR protection field is blank in original rendered PDF; not extraction loss.')],
        entries=entries,inventory_scope='All 23 Table3 entries transcribed and detail sections read; source inventory only.',
        exact_device_semantic_closure=False,execution_permission='none',
        remaining=['Exact-instance SDK workaround audit','Family-only mode intersection','Board/revision/context binding',
                   'Missing PWR field and source register spellings','Implementation/build/hardware qualification'])


def verify(bundle):
    # Bind all retained wording and scope; independent source review establishes semantics separately.
    assert bundle==build()
    assert bundle['publication']=='unpublished' and bundle['execution_permission']=='none'
    assert bundle['exact_device_semantic_closure'] is False
    assert bundle['target']==json.loads((ROOT/'candidate.json').read_text(encoding='utf-8'))['target']
    assert bundle['base_candidate_sha256']==sha(ROOT/'candidate.json')
    source=bundle['source']
    assert sha(source['pdf'])==source['sha256'] and sha(source['extraction'])==source['extraction_sha256']
    raw=json.loads(Path(source['extraction']).read_text(encoding='utf-8'))
    assert raw['document_sha256']==source['sha256']
    pages={p['page']:p['text'] for p in raw['pages']}
    summary='\n'.join(pages[p] for p in [2,3])
    assert set(re.findall(r'\b2\.\d+\.\d+\b',summary))==set(STATUS)
    assert len(bundle['entries'])==23 and {e['section'] for e in bundle['entries']}==set(STATUS)
    for key,e in bundle['evidence'].items():
        assert key==str(e['page']) and e['text']==pages[e['page']]
        assert hashlib.sha256(e['text'].encode()).hexdigest()==e['text_sha256']
    for e in bundle['entries']:
        assert e['revision_status']==dict(zip(['Z','Y'],STATUS[e['section']]))
        assert e['section'] in '\n'.join(bundle['evidence'][str(p)]['text'] for p in e['detail_pages'])
        assert e['review']=='candidate' and e['implementation']=='not_audited'
        assert e['build']=='not_tested' and e['hardware']=='not_observed' and e['execution_permission']=='none'
    adc=next(e for e in bundle['entries'] if e['section']=='2.3.1')
    assert adc['exact_device_applicability']=='unresolved_family_applicability_exact_C542_has_one_ADC_do_not_infer_ADC2_or_dual_mode'
    assert [v['page'] for v in bundle['visual_table_checks']]==[2,3]
    for v in bundle['visual_table_checks']:assert sha(v['path'])==v['sha256']
    assert len(bundle['visual_source_defects'])==1 and bundle['visual_source_defects'][0]['page']==8
    for v in bundle['visual_source_defects']:assert sha(v['path'])==v['sha256']


if __name__=='__main__':
    bundle=build();verify(bundle)
    fixtures=[]
    for name,change in [
        ('dropped_entry',lambda b:b['entries'].pop()),
        ('wrong_COMP_Y_status',lambda b:next(e for e in b['entries'] if e['section']=='2.4.1')['revision_status'].update(Y='N')),
        ('inferred_ADC2',lambda b:next(e for e in b['entries'] if e['section']=='2.3.1').update(exact_device_applicability='supported')),
        ('false_hardware',lambda b:b['entries'][0].update(hardware='passed')),
        ('false_closure',lambda b:b.update(exact_device_semantic_closure=True)),
        ('unbound_detail',lambda b:b['entries'][0].update(detail_pages=[11]))]:
        changed=copy.deepcopy(bundle);change(changed)
        try:verify(changed)
        except (AssertionError,KeyError):fixtures.append({'id':name,'result':'rejected'})
        else:raise AssertionError(name)
    (ROOT/'errata.json').write_text(json.dumps(bundle,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
    (ROOT/'errata-acceptance.json').write_text(json.dumps(dict(bundle_sha256=sha(ROOT/'errata.json'),
        author_sha256=sha(__file__),inventory_entries=23,negative_fixtures=fixtures,
        qualification='Source inventory consistency only; no implementation or workaround validation.'),indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'entries':23,'negative_fixtures':len(fixtures),'sha256':sha(ROOT/'errata.json')}))
