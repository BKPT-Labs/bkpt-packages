# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Rebuild an unpublished C542 evidence candidate; never writes product catalogs."""
import hashlib
import json
import re
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DATA = Path('D:/Projects/BKPT-Data')
SOURCE = DATA / 'firmware/sources/st/c5'
SDK = DATA / 'sdk/stm32c5-hal2-2.1.0'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def dump(name, data):
    (ROOT / name).write_text(json.dumps(data, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


def build():
    sources, excerpts = {}, {}
    documents = {}
    for doc, revision in [('DS15135', '2'), ('RM0522', '1'), ('ES0676', '2')]:
        path = SOURCE / f'documents/{doc}.pages.json'
        raw = json.loads(path.read_text(encoding='utf-8'))
        pdf = SOURCE / f'documents/{doc}.pdf'
        assert digest(pdf.read_bytes()) == raw['document_sha256']
        sources[doc] = dict(path=str(pdf), sha256=raw['document_sha256'], version=revision,
                            authority='manufacturer', extraction_path=str(path),
                            extraction_sha256=digest(path.read_bytes()), extractor=raw['extractor'])
        documents[doc] = {p['page']: p for p in raw['pages']}
    for key, path in [('svd', SOURCE / 'pack-2.1.0/device.svd'),
                      ('header', SDK / 'stm32c5xx_dfp/Include/stm32c542xx.h')]:
        sources[key] = dict(path=str(path), sha256=digest(path.read_bytes()), version='2.1.0',
                            authority='manufacturer_sdk', upstream_archive_identity='not_verified_here')
    visual_evidence = []
    for name, doc, number in [('ds-table-check','DS15135',5), ('ds-pin-check','DS15135',37),
                               ('dma-instance-check','RM0522',383), ('uart-modes-check','DS15135',28),
                               ('dma-conflict-check','DS15135',13)]:
        path = ROOT/f'{name}.png'
        visual_evidence.append(dict(path=str(path),sha256=digest(path.read_bytes()),
                                    document=doc,document_sha256=sources[doc]['sha256'],page=number,
                                    reviewer='Codex author',state='visual_checked',
                                    method='Poppler pdftoppm page rendering; no desktop control'))

    def page(doc, number):
        key = f'{doc}:p{number}'
        raw = documents[doc][number]
        excerpts[key] = dict(source=doc, locator={'page': number}, text=raw['text'],
                             text_sha256=digest(raw['text'].encode()), warnings=raw['warnings'],
                             visual_check='author_checked' if (doc, number) in
                             [('DS15135', 5), ('DS15135',13), ('DS15135', 28), ('DS15135', 37), ('RM0522', 383)] else 'not_checked')
        return key

    target = dict(device='STM32C542RCT6', device_group='STM32C542xx', package='LQFP64',
                  silicon_revision='unknown', sdk='stm32c5-hal2-2.1.0')
    states = dict(review='candidate', implementation='not_authored_in_this_bundle',
                  build='not_tested', hardware='not_tested', closure='open')
    claims = []

    def claim(key, subject, predicate, value, refs, limits, availability='source_known'):
        claims.append(dict(id=key, subject=subject, predicate=predicate, value=value,
                           evidence=[page(*r) for r in refs], applicability=target.copy(),
                           availability=availability, states=states.copy(), limitations=limits,
                           errata_coverage='incomplete'))

    claim('cordic.presence', 'CORDIC', 'device_presence', True, [('DS15135', 2), ('DS15135', 5)],
          ['Pinned SVD peripheral node is absent; header declares register block and IRQ.',
           'No executable adapter or numerical/precision qualification is supplied.'])
    claim('gpio.count', 'GPIO', 'package_gpio_count', 52, [('DS15135', 5)],
          ['Package bonding does not establish board accessibility or electrical suitability.'])
    # Explicit visual transcription of Figure 10, not a name-derived pin domain.
    pads = ('PE2 PC13 PC14 PC15 PH0 PH1 NRST PC0 PC1 PC2 PC3 VREF- VREF+ PA0 PA1 PA2 '
            'PA3 VSS VDD PA4 PA5 PA6 PA7 PC4 PC5 PB0 PB1 PB2 PB10 VCAP VSS VDD '
            'PB12 PB13 PB14 PB15 PC6 PC7 PC8 PC9 PA8 PA9 PA10 PA11 PA12 PA13 VSS VDD '
            'PA14 PA15 PC10 PC11 PC12 PD2 PB3 PB4 PB5 PB6 PB7 PH2 PB8 PB9 VSS VDD').split()
    assert len(pads) == 64
    pins = [dict(pad=i, signal=s, evidence=[page('DS15135', 37)],
                 electrical='unknown', board_access='unknown', alternate_functions='unreviewed',
                 review='candidate') for i, s in enumerate(pads, 1)]
    functions = {'PH0':['OSC_IN'], 'PH1':['OSC_OUT'], 'PH2':['BOOT0'],
                 'PC14':['OSC32_IN'], 'PC15':['OSC32_OUT'], 'PA13':['JTMS','SWDIO'],
                 'PA14':['JTCK','SWCLK'], 'PA15':['JTDI'], 'PB3':['JTDO','TRACESWO'], 'PB4':['NJTRST'],
                 'PC13':['LSE_crystal_interaction_requires_revision_context']}
    for pin in pins:
        pin['device_function_constraints'] = functions.get(pin['signal'], [])
        pin['reservation_state'] = 'requires_context' if pin['device_function_constraints'] else 'not_assessed'
        if pin['device_function_constraints']:
            pin['evidence'] += [page('RM0522',341), page('RM0522',342)]
    assert sum(bool(re.fullmatch(r'P[A-H]\d+', p['signal'])) for p in pins) == 52
    for mode in ['input', 'output_push_pull', 'output_open_drain', 'analog',
                 'alternate_function_push_pull', 'alternate_function_open_drain']:
        claim(f'gpio.mode.{mode}', 'GPIO', 'mode', mode, [('RM0522', 339), ('RM0522', 342)],
              ['Requires bonded pad and per-pin electrical/function evidence; not every analog function exists on every pad.'])
    claim('gpio.af.exclusion', 'GPIO', 'max_simultaneous_alternate_functions_per_pin', 1,
          [('RM0522', 342)], ['Additional RTC/PWR/RCC functions can override ordinary GPIO configuration.'])
    claim('gpio.bsrr.atomic', 'GPIO', 'atomic_set_reset', {'set_wins_overlap': True, 'zero_no_effect': True},
          [('RM0522', 343)], ['Atomic bit writes do not establish application-level ownership.'])
    claim('gpio.lock', 'GPIO', 'configuration_lock', {'release': 'MCU_or_peripheral_reset', 'access_bits': 32},
          [('RM0522', 343)], ['The exact write/read sequence still needs implementation review.'])
    claim('exti.mux', 'EXTI', 'gpio_mux_lines', list(range(16)), [('RM0522', 450)],
          ['One selected port per line; package pin availability is a separate condition.'])
    claim('exti.trigger_masks', 'EXTI', 'mux_output_independent_of_cpu_masks', True, [('RM0522', 450)],
          ['CPU delivery and peripheral-trigger delivery are separate edges.'])
    claim('exti.pending', 'EXTI', 'configurable_pending_clear', 'write_one', [('RM0522', 451)],
          ['Rising/falling pending bits are separate; direct events clear in their originating peripheral.'])
    claim('exti.direct', 'EXTI', 'direct_event_contract',
          {'pending_owner': 'origin_peripheral', 'software_trigger': False}, [('DS15135', 14), ('RM0522', 447)],
          ['This does not classify all numbered lines; source availability must be checked per line.'])
    claim('nvic.priority', 'NVIC', 'priority_bits', 4, [('DS15135', 14)],
          ['Priority grouping, SDK encoding, IRQ ownership and vector handlers need separate validation.'])
    claim('nvic.channels', 'NVIC', 'declared_maskable_channels', 88, [('DS15135', 14)],
          ['Not 88 valid peripheral IRQ assignments; reserved vector positions remain reserved.'])
    for instance in ['LPDMA1', 'LPDMA2']:
        claim(f'{instance}.channels', instance, 'channel_indices', [0, 1, 2, 3],
              [('DS15135', 13), ('RM0522', 383)],
              ['DS15135 Table 2 says x=0 to 4; conflicts with its own four-channel statement and RM Table 86.',
               'Do not silently normalize the inconsistent table; channel 4 remains rejected by candidate contract.'])
        for mode in ['peripheral_to_memory', 'memory_to_peripheral', 'memory_to_memory', 'peripheral_to_peripheral']:
            claim(f'{instance}.{mode}', instance, 'transfer_direction_class', mode, [('DS15135', 13)],
                  ['Does not certify any concrete endpoint pair. Bus reachability, widths, side effects and request pacing remain required.'])
    claim('dma.requests.shared', 'LPDMA1/2', 'same_request_and_trigger_connections', True, [('RM0522', 383)],
          ['Only implemented peripherals/channels can supply requests or triggers.'])
    claim('dma.abort', 'LPDMA1/2', 'abort_order', ['request_suspend', 'observe_SUSPF', 'reset_channel'],
          [('RM0522', 392)], ['Pending peripheral/DMA interrupts and callbacks need adapter-owned synchronization before buffer release.'])
    claim('dma.block_request', 'LPDMA1/2', 'block_request_required', 'lptim1_ue_dma', [('RM0522', 387)],
          ['BREQ must be set. Other requests require their own reviewed restrictions.'])
    claim('dma.early_termination', 'LPDMA1/2', 'candidate_early_termination',
          {'channels': [0], 'request': 'i3c1_rx_dma', 'PFREQ': 1},
          [('RM0522', 383), ('RM0522', 387), ('RM0522', 388)],
          ['Derived intersection: RM family early-termination channels 0/7 with C542 channels 0..3.',
           'Source table visual review and peripheral SDK lifecycle review remain required.'])
    claim('dma.link.window', 'LPDMA1/2', 'linked_list_address_window_bytes', 65536,
          [('RM0522', 394)], ['Entire next item must fit in the window; alignment and memory reachability are additional constraints.'])
    claim('dma.restart', 'LPDMA1/2', 'restart_reprogram_registers', ['CxBR1', 'CxSAR', 'CxDAR'],
          [('RM0522', 392), ('RM0522', 393)], ['After confirmed suspend/reset, before enable; SDK and callback ownership still unreviewed.'])
    claim('rcc.gate.delay', 'RCC', 'enable_to_access_bus_cycles', 2, [('RM0522', 271)],
          ['Applies to the peripheral bus clock, not an arbitrary CPU delay loop.'])
    claim('rcc.rtc.clock', 'RTC/TAMP', 'clock_selection',
          {'sources': ['HSE_1MHz', 'LSE', 'LSI'], 'change_requires': 'RTC_domain_reset', 'PCLK_gte_RTCCLK': True},
          [('RM0522', 271)], ['Board oscillator availability and Standby retention depend on selected source.'])
    claim('timer.clock.conflict', 'TIM6/TIM7', 'clock_relation',
          {'RCC_section': 'HCLK', 'timer_section': 'tim_pclk'}, [('RM0522', 271), ('RM0522', 1261)],
          ['Unresolved when HCLK differs from APB clock; do not widen fixed equal-clock foundation.'], 'conflicting')
    # Table 9 was visually checked, including its X and dash cells and footnotes.
    serial_modes = {
        'hardware_flow_control': [True,True,True], 'continuous_DMA': [True,True,True],
        'multiprocessor': [True,True,True], 'synchronous': [True,False,False],
        'smartcard': [True,False,False], 'single_wire_half_duplex': [True,True,True],
        'IrDA': [True,True,False], 'LIN': [True,True,False], 'stop_wakeup': [True,True,True],
        'receiver_timeout': [True,True,False], 'Modbus': [True,True,False],
        'autobaud': [True,True,False], 'driver_enable': [True,True,True], 'FIFO': [True,True,True]}
    for instances, column in [(['USART1','USART2'],0), (['UART4','UART5'],1), (['LPUART1'],2)]:
        for instance in instances:
            for mode, supported in serial_modes.items():
                claim(f'{instance}.{mode}', instance, 'device_mode_available',
                      {'mode': mode, 'available': supported}, [('DS15135', 28)],
                      ['Datasheet declaration only; no baud/pin/clock/errata/SDK lifecycle qualification.',
                       'False reflects a dash in Table 9, not missing evidence.'])
    # Datasheet-specific declarations; these remain candidate modes, not usable generators.
    specs = [
        ('ICACHE', 8, ['invalidate', 'performance_counters', 'direct_mapped', 'set_associative']),
        ('CRS', 12, ['automatic_trim', 'manual_trim', 'USB_SOF_sync', 'LSE_sync', 'external_sync']),
        ('CRC', 15, ['8_bit_input', '16_bit_input', '32_bit_input']),
        ('DAC1', 17, ['buffered', 'unbuffered', 'sample_hold', 'DMA', 'internal_connection', 'noise', 'triangle']),
        ('COMP1/COMP2', 17, ['hysteresis', 'blanking', 'timer_capture_route', 'timer_break_route', 'EXTI_wakeup']),
        ('OPAMP1', 17, ['standalone', 'follower', 'inverting_PGA', 'noninverting_PGA']),
        ('RNG', 18, ['health_tests', 'automatic_low_power', '32_bit_single_access']),
        ('AES', 19, ['ECB', 'CBC', 'CTR', 'GCM', 'GMAC', 'CCM', 'single_transfer_DMA']),
        ('HASH', 19, ['SHA1', 'SHA224', 'SHA256', 'HMAC']),
        ('WWDG', 23, ['window_refresh', 'early_wakeup_interrupt']),
        ('RTC', 24, ['calendar', 'binary', 'alarms', 'timestamp', 'calibration', 'wakeup_timer']),
        ('TAMP', 24, ['passive_edge', 'passive_level', 'filter', 'confirmed', 'potential', 'backup_registers']),
        ('FDCAN', 32, ['classic_CAN', 'CAN_FD', 'loopback', 'power_down']),
        ('DBGMCU', 33, ['SWD', 'JTAG']),
        ('FLASH', 9, ['read_while_other_bank_write', 'OTP', 'ECC', 'RDP', 'write_protection', 'hide_protection']),
        ('PWR', 9, ['supply_domains', 'LDO', 'supervision', 'low_power', 'privileged_protection']),
        ('ADC1', 16, ['single', 'continuous', 'scan', 'software_trigger', 'hardware_trigger']),
        ('TIM1/TIM8', 21, ['input_capture', 'output_compare', 'PWM', 'complementary_deadtime', 'break', 'encoder', 'synchronization']),
        ('TIM6/TIM7', 22, ['time_base', 'DAC_trigger', 'update_interrupt_DMA']),
        ('LPTIM1', 22, ['one_shot', 'continuous', 'PWM', 'input_capture', 'encoder', 'low_power']),
        ('I2C1', 25, ['controller', 'target', 'multicontroller', '7_bit', '10_bit', 'standard', 'fast', 'fast_plus']),
        ('I3C1', 26, ['SDR_primary_controller', 'SDR_secondary_controller', 'SDR_target']),
        ('SPI1/SPI2', 31, ['master', 'slave', 'simplex', 'half_duplex', 'full_duplex']),
        ('USB_DRD_FS', 33, ['host', 'device', 'isochronous', 'double_buffer_bulk_isochronous'])]
    for subject, number, modes in specs:
        for mode in modes:
            refs = [('DS15135', number)] + ([('DS15135', 20)] if subject == 'TIM1/TIM8' else [])
            claim(f'{subject}.{mode}', subject, 'declared_mode_candidate', mode, refs,
                  ['Source overview only: exact instance restrictions, pins, clocking, errata and SDK lifecycle remain open.'])

    # Errata conditions are distinct from the unknown physical target revision.
    errata = [
        ('lse.pc13', 'RCC/GPIO', [4,5], ['Z','Y'],
         {'package':'LQFP64', 'LSE_mode':'crystal', 'PC13':'toggling_input_or_output'},
         'Avoid toggling PC13 while LSE crystal is used. Bypass is unaffected by this erratum only; analog bypass has separate erratum 2.2.1. Digital bypass requires LSEBYP=1 and LSEEXT=1.'),
        ('lse.analog_bypass', 'RCC', [4], ['Z','Y'], {'LSEBYP':1,'LSEEXT':0},
         'Do not use analog LSE bypass; select digital bypass with LSEBYP=1 and LSEEXT=1.'),
        ('lse.low_drive', 'RCC', [5], ['Z','Y'], {'LSEDRV':'00'}, 'Low-drive mode is forbidden by the erratum.'),
        ('comp.concurrent', 'COMP1/COMP2', [6], ['Z'], {'COMP1_EN':True,'COMP2_EN':True},
         'Simultaneous operation is not supported; each operates as specified individually.'),
        ('lptim.disable', 'LPTIM1', [6], ['Z','Y'], {'operation':'disable'},
         'Use RCC peripheral reset rather than clearing ENABLE to avoid the Stop-entry interrupt hazard.'),
        ('adc.smptrig', 'ADC1', [5], ['Z','Y'], {'sequence':'SMPTRIG_set_regular_start_stop_SMPTRIG_clear'},
         'If SMPTRIG is set, disable ADC, clear SMPTRIG, re-enable before injected conversion.')]
    for key, subject, pages, revisions, conditions, mitigation in errata:
        claim('errata.'+key, subject, 'conditional_erratum',
              {'section_key':key, 'silicon_revisions':revisions, 'conditions':conditions, 'mitigation':mitigation},
              [('ES0676',1),('ES0676',2)]+[('ES0676',p) for p in pages],
              ['Revision table visual/independent review pending; actual silicon revision unknown.',
               'Workaround text is a candidate requirement, not implemented or validated.'])

    svd = ET.parse(sources['svd']['path']).getroot()
    inventory = []
    for peripheral in svd.findall('./peripherals/peripheral'):
        name = peripheral.findtext('name')
        inventory.append(dict(id=name, source_state='svd_declared',
            locator=f"/device/peripherals/peripheral[name='{name}']", source='svd',
            description=peripheral.findtext('description'), derived_from=peripheral.get('derivedFrom'),
            base_address=peripheral.findtext('baseAddress'), semantic_presence='needs_DS_RM_binding',
            states=states.copy(), modes='open', package_pins='unknown', sdk_integration='unreviewed'))
    assert len(inventory) == 51 and not any(i['id']=='CORDIC' for i in inventory)
    inventory.append(dict(id='CORDIC', source_state='DS_and_header_declared_SVD_node_missing',
                          semantic_presence='source_known', evidence=['DS15135:p2', 'DS15135:p5'], states=states.copy()))
    # These are work domains, deliberately not additional physical-peripheral counts.
    work_domains = ['CPU/FPU/DSP', 'NVIC', 'SysTick', 'MPU', 'SRAM/bus_matrix', 'boot/system_memory',
                    'option_bytes/lifecycle', 'electronic_signature', 'package/electrical',
                    'board_routing', 'interconnect', 'silicon_errata']
    subject_groups = {
        'GPIO':['GPIOA','GPIOB','GPIOC','GPIOD','GPIOE','GPIOH'],
        'LPDMA1/2':['LPDMA1','LPDMA2'], 'TIM1/TIM8':['TIM1','TIM8'],
        'TIM6/TIM7':['TIM6','TIM7'], 'SPI1/SPI2':['SPI1','SPI2'],
        'COMP1/COMP2':['COMP1','COMP2'], 'RCC/GPIO':['RCC','GPIO'],
        'RTC/TAMP':['RTC','TAMP'], 'FDCAN':['FDCAN1','FDCAN2']}
    aliases = {
        'LPUART1':{'svd_node':'LPUART','relation':'SDK_instance_alias','review':'candidate'},
        'COMP1':{'svd_node':'COMP12','relation':'subinstance_of_shared_block','review':'candidate'},
        'COMP2':{'svd_node':'COMP12','relation':'subinstance_of_shared_block','review':'candidate'},
        'ADCC12':{'sdk_name':'ADC12_COMMON','relation':'common_register_block_not_ADC2','review':'candidate'}}
    excluded_family = ['XSPI', 'DLYB', 'CCB', 'SAES', 'PKA', 'ETH', 'TIM3', 'TIM4', 'TIM5', 'TIM16', 'TIM17']
    # Exact header IRQ declarations are SDK syntax evidence only, including comments.
    header = Path(sources['header']['path']).read_text(encoding='utf-8')
    sdk_excerpts = []
    for label, start, end in [('cordic_irq',122,122),('cordic_registers',296,303),
                              ('cordic_base',1234,1234),('cordic_instance',1355,1355),
                              ('comparator_instances',1313,1315),('lpuart_base',1215,1215),
                              ('lpuart_instance',1336,1336),('adc_common',1362,1362),
                              ('adc_instances',14015,14015)]:
        text = '\n'.join(header.splitlines()[start-1:end])
        sdk_excerpts.append(dict(id=label,source='header',start_line=start,end_line=end,
                                 text=text,text_sha256=digest(text.encode()),review='candidate'))
    next(c for c in claims if c['id']=='cordic.presence')['sdk_evidence'] = ['cordic_irq','cordic_registers','cordic_base','cordic_instance']
    next(i for i in inventory if i['id']=='CORDIC')['sdk_evidence'] = ['cordic_irq','cordic_registers','cordic_base','cordic_instance']
    irqs = []
    for line, text in enumerate(header.splitlines(), 1):
        match = re.match(r'\s*(\w+_IRQn)\s*=\s*(-?\d+),?\s*(.*)', text)
        if match:
            irqs.append(dict(symbol=match[1], number=int(match[2]), comment=match[3],
                             source='header', line=line, kind='SDK_declaration', review='candidate'))
    bundle = dict(schema='bkpt.c5-reference.candidate/1', publication='unpublished', target=target,
                  board_context={'board':'NUCLEO-C542RC','routing_review':'not_reviewed'},
                  sources=sources, excerpts=excerpts, inventory=inventory, claims=claims, package_pads=pins,
                  visual_evidence=visual_evidence,sdk_excerpts=sdk_excerpts,
                  subject_groups=subject_groups, source_aliases=aliases,
                  irq_declarations=irqs, work_domains=[dict(id=x, closure='open') for x in work_domains],
                  family_only_investigation=[dict(id=x, availability='unknown', reason='RM chapter alone is not device presence') for x in excluded_family],
                  conflicts=[dict(id='cordic_svd', state='source_omission_identified',
                                  detail='Pinned SVD has no peripheral node, DS and header declare CORDIC. Original pack archive provenance not revalidated.'),
                             dict(id='dma_channel_table', state='conflicting',
                                  evidence=[page('DS15135', 13), page('RM0522', 383)],
                                  detail='DS Table 2 x=0..4 versus four channels per instance in DS prose and RM Table 86 x=0..3.'),
                             dict(id='exti_event_counts', state='scope_difference_not_resolved',
                                  evidence=[page('DS15135', 14), page('RM0522', 445)],
                                  detail='DS says 37 inputs, family RM says 40. Do not assume every line below either bound is implemented.'),
                             dict(id='nvic_count',state='count_semantics_unresolved',evidence=[page('DS15135',14)],
                                  detail='Datasheet says 88 maskable channels; SDK IRQ enumeration includes value 88. Reserved positions and count semantics need reconciliation.'),
                             dict(id='fdcan_count',state='conflicting',evidence=[page('DS15135',5),page('DS15135',32)],
                                  detail='DS Table1 lists two FDCAN instances while section3.28 says one CAN module. Preserve distinct source scopes; do not infer shared RAM capacity or ownership.')],
                  baseline=dict(path=str(DATA/'architecture/stm32c5-transfer-ops-v1.sqlite'),
                                sha256='d27cedf5e3d2d436af5c4cc7cdf3bbedc06b7a684c40555128956fc19517afa8',
                                status='external_immutable_baseline_not_requalified_here'),
                  closure=dict(complete_domains=0, complete_device=False,
                               rule='No count of extracted records establishes semantic or executable coverage.'))
    # page() calls above can extend excerpts after its initial insertion in bundle.
    dump('candidate.json', bundle)
    dump('manifest.json', dict(candidate_sha256=digest((ROOT/'candidate.json').read_bytes()),
                               author_sha256=digest(Path(__file__).read_bytes()),
                               contracts_sha256=digest((ROOT/'contracts.json').read_bytes()),
                               transcription_sha256=digest((ROOT/'pin-transcription.json').read_bytes()),
                               counts=dict(svd_nodes=51, additional_source_known_blocks=1, claims=len(claims),
                                           package_pads=len(pins), irq_declarations=len(irqs),
                                           complete_domains=0), publication='unpublished'))
    return bundle


if __name__ == '__main__':
    build()
    print((ROOT/'manifest.json').read_text(encoding='utf-8'))
