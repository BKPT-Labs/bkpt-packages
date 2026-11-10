# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Author bounded, unpublished C542 composition contracts from pinned evidence.

This is an external authoring/acceptance tool, never a firmware runtime or publisher.
"""
import copy
import hashlib
import json
import re
from fractions import Fraction
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DATA = Path('D:/Projects/BKPT-Data')
SDK = DATA/'sdk/stm32c5-hal2-2.1.0'
DOCS = DATA/'firmware/sources/st/c5/documents'


def sha(value):
    return hashlib.sha256(value).hexdigest()


def write(name, value):
    (ROOT/name).write_text(json.dumps(value,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')


def build(write_output=True):
    base = json.loads((ROOT/'candidate.json').read_text(encoding='utf-8'))
    evidence, sources, pages = {}, {}, {}
    for name in ['DS15135','RM0522','ES0676']:
        p=DOCS/f'{name}.pages.json'
        raw=json.loads(p.read_text(encoding='utf-8'))
        sources[name]=dict(path=str(DOCS/f'{name}.pdf'),sha256=raw['document_sha256'],
                           extraction_path=str(p),extraction_sha256=sha(p.read_bytes()))
        assert sha(Path(sources[name]['path']).read_bytes())==sources[name]['sha256']
        pages[name]={v['page']:v['text'] for v in raw['pages']}

    def page(number, doc='RM0522'):
        key=f'{doc}:p{number}'
        text=pages[doc][number]
        evidence[key]=dict(source=doc,page=number,text=text,text_sha256=sha(text.encode()),
                           visual_validation='not_claimed_here')
        return key

    def lines(key, relative, first, last):
        p=SDK/relative
        sources[relative]=dict(path=str(p),sha256=sha(p.read_bytes()),kind='SDK_source',version_context='pinned_HAL2_2.1.0')
        text='\n'.join(p.read_text(encoding='utf-8').splitlines()[first-1:last])
        evidence[key]=dict(source=relative,start_line=first,end_line=last,text=text,text_sha256=sha(text.encode()))
        return key

    h='stm32c5xx_dfp/Include/stm32c542xx.h'
    extic='stm32c5xx_drivers/hal/stm32c5xx_hal_exti.c'
    extih='stm32c5xx_drivers/hal/stm32c5xx_hal_exti.h'
    ll='stm32c5xx_drivers/ll/stm32c5xx_ll_dma.h'
    refs={
        'gpio_init':lines('sdk.gpio_init','stm32c5xx_drivers/hal/stm32c5xx_hal_gpio.c',93,115),
        'exti_lifecycle':lines('sdk.exti_lifecycle',extic,60,93),
        'exti_config':lines('sdk.exti_config',extic,318,400),
        'exti_enable':lines('sdk.exti_enable',extic,495,537),
        'exti_irq':lines('sdk.exti_irq',extic,745,807),
        'exti_callback':lines('sdk.exti_callback',extic,809,842),
        'exti_disable':lines('sdk.exti_disable',extic,540,564),
        'exti_deinit':lines('sdk.exti_deinit',extic,261,308),
        'exti_types':lines('sdk.exti_types',extih,117,181),
        'exti_irqs':lines('sdk.exti_irqs',h,72,87),
        'dma_trigger_modes':lines('sdk.dma_trigger_modes',ll,296,317),
        'dma_exti_triggers':lines('sdk.dma_exti_triggers',ll,640,648),
        'dma2_exti_triggers':lines('sdk.dma2_exti_triggers',ll,703,710),
        'dma_trigger_scope':lines('sdk.dma_trigger_scope',ll,663,699)}
    refs['rcc_timer']=lines('sdk.rcc_timer','stm32c5xx_drivers/hal/stm32c5xx_hal_rcc.c',3653,3667)
    refs['tim_clock']=lines('sdk.tim_clock','stm32c5xx_drivers/hal/stm32c5xx_hal_tim.c',3855,3862)
    refs['tim_instances']=lines('sdk.tim_instances',h,14309,14317)
    clock=Path('C:/Users/Eddie/AppData/Local/stm32cube/packs/STMicroelectronics/stm32c5xx_dfp/2.1.0/Descriptors/clock/STM32C542(C-K-R)C(T-U)x_clock.json')
    sources['clock_descriptor']=dict(path=str(clock),sha256=sha(clock.read_bytes()),version='4.0.0',
                                     qualification='literal_descriptor_not_measured_clock')
    raw=json.loads(clock.read_text(encoding='utf-8'))
    for category,index in [('elements',48),('elements',49),('transitions',50),('transitions',52)]:
        key=f'descriptor.{category}.{index}'
        value=raw['tree'][category][index]
        text=json.dumps(value,sort_keys=True,separators=(',',':'))
        evidence[key]=dict(source='clock_descriptor',pointer=f'/tree/{category}/{index}',value=value,
                           text=text,text_sha256=sha(text.encode()))
    rules=[]
    def rule(key,predicate,value,refs,conditions=None):
        rules.append(dict(id=key,predicate=predicate,value=value,evidence=refs,
                          conditions=conditions or [],review='candidate',implementation='not_integrated',
                          hardware='not_observed',errata_coverage='incomplete'))
    rule('dma.channel.domain','channel_domain',{'LPDMA1':[0,1,2,3],'LPDMA2':[0,1,2,3]},
         [page(383),page(13,'DS15135')],['DS Table2 x=0..4 conflict preserved in base candidate.'])
    rule('dma.width','single_width_bytes',[1,2,4],[page(408),page(430)])
    rule('dma.alignment','address_mod_width_equals',0,[page(408),page(430)])
    rule('dma.block','source_block',{'min_nonzero':1,'max_bytes':65535,'multiple_of':'source_width'},[page(434)])
    rule('dma.request.exclusion','active_hardware_request_capacity',1,[page(410),page(433)],
         ['Across-controller reservation is conservative author policy, not proven hardware arbitration scope.',
          'Hardware request identities are outside the reviewed checker domain; SWREQ=0 requires future finite selector binding.',
          'Time-sharing requires a separately reviewed schedule; this contract reserves for full lifetime.'])
    rule('dma.trigger.broadcast','trigger_fanout','multiple_channels_allowed',[page(415)])
    rule('dma.trigger.overrun','trigger_overrun',{'pending_hits':1,'next_hit':'lost_with_TOF','channel_auto_disables':False},[page(414),page(432)])
    rule('dma.trigger.granularity','TRIGM',{'block':0,'link':2,'single':3},[page(432),refs['dma_trigger_modes']])
    rule('dma.trigger.polarity','TRIGPOL',{'masked':0,'rising':1,'falling':2},[page(432),refs['dma_trigger_modes']],
         ['RM p432 TRIGSEL prose says active for 00, contradicting TRIGPOL/TRIGM and SDK comments; use only corroborated 01/10 in candidate profiles.'])
    rule('dma.abort','stop_order',['set_SUSP','observe_SUSPF','set_RESET','reprogram_before_restart'],[page(392),page(393)],
         ['Adapter must also synchronize callbacks and verify physical release before releasing memory.'])
    rule('dma.bus','reachable_bus_classes',['FLASH','SRAM1','SRAM2','AHB1/APB1/APB2','AHB2','AHB3/APB3'],
         [page(87),page(88),page(89)],['Intersect family bus statement with C542 implementation; AHB4 excluded.',
                                    'Bus reachability is not permission to read/write every address.'])
    rule('gpio.input.register','IDR_access','read_only',[page(343)])
    rule('gpio.software_dma','software_request_endpoints','GPIO_to_or_from_memory_explicitly_described',[page(410)])
    rule('exti.mux','selected_port_per_numbered_line',1,[page(450)])
    rule('exti.delivery','mux_output_independent_of_IMR_EMR',True,[page(450)],
         ['Disabling CPU interrupt/event delivery is not quiescence for DMA consumers.'])
    rule('exti.pending','configurable_pending_clear','write_1',[page(451),refs['exti_irq']],
         ['Rising and falling pending states are distinct; direct events use originating peripheral flags.'])
    rule('exti.callback','callback_registration_requires',{'macro':'USE_HAL_EXTI_REGISTER_CALLBACKS','value':1,
         'states':['INIT','IDLE'],'signature':'void (*)(hal_exti_handle_t *, hal_exti_trigger_t)'},
         [refs['exti_types'],refs['exti_callback']],
         ['State checks are ASSERT_DBG_STATE, not release-build enforcement; adapter must preflight state.'])
    rule('exti.release','release_semantics',{'Disable':'masks_IT_and_event_only',
         'DeInit':'also_disables_edges_clears_pending_and_resets_GPIO_mux_to_port_A'},
         [refs['exti_disable'],refs['exti_deinit']],
         ['Quiesce all consumers before DeInit; CPU masking alone does not release DMA consumers.',
          'DeInit lacks a state assertion and resets handle state; SetConfig/Enable/Disable state assertions are debug-only.'])
    rule('exti.callback.order','IRQ_clears_flags_before_callback',True,[refs['exti_irq']],
         ['Callback may report both edges; no edge count or ordering guarantee.'])
    rule('gpio.clock','clock_before_configuration',True,[refs['gpio_init'],page(271)],
         ['HAL clock-enable model must be pinned; manual gate requires documented two bus cycles.'])
    rule('timer.kernel.models','non_LPTIM_clock_models',{'RM_RCC':'HCLK','HAL_calculation':'HCLK','descriptor':'HCLK',
         'RM_TIM6_7':'PCLK1'},[page(271),page(1261),refs['rcc_timer'],refs['tim_clock'],refs['tim_instances'],
                            'descriptor.transitions.50','descriptor.transitions.52'],
         ['TIM6/7 unequal-HCLK/PCLK1 remains conflicting; no majority-vote resolution.',
          'SDK frequency getter computes from settings, not measured clock or readiness.'])
    rule('timer.basic.domain','basic_timer_candidate_domain',{'instances':['TIM6','TIM7'],'hclk_hz':144000000,
         'pclk1_hz':144000000,'psc_min':0,'psc_max':65535,'arr_min':1,'arr_max':65535,'DITHEN':0},
         [page(1259),page(1261),page(1262),page(1278),page(1279)],
         ['144 MHz is an author-selected foundation constraint, not a maximum-frequency claim.',
          'Nominal fixed-clock foundation only; electrical frequency tolerance and clock switching are not qualified.'])
    rule('timer.prescaler.load','PSC_effective_on','update_event',[page(1262),page(1278)])
    rule('timer.UG.side_effect','UG_can_generate_IRQ_DMA_if',{'URS':0,'UDIS':0},[page(1274),page(1277)],
         ['Initializing buffered PSC/ARR must use an explicit pending/request policy before enabling consumers.'])
    rule('timer.trigger.clock','TRGO_consumer_clock_requires',{'enabled_before_trigger':True,'no_on_the_fly_change':True},[page(1275)])

    pins=[]
    for pin in base['package_pads']:
        m=re.fullmatch(r'P([A-H])(\d+)',pin['signal'])
        if not m:continue
        line=int(m[2])
        irq=next(v for v in base['irq_declarations'] if v['symbol']==f'EXTI{line}_IRQn')
        pins.append(dict(pin=pin['signal'],pad=pin['pad'],port=m[1],exti_line=line,
                         irq=irq['number'],irq_symbol=irq['symbol'],
                         dma_trigger_selector=line if line<8 else None,
                         context_constraints=pin['device_function_constraints'],
                         evidence=['base:package_pads',page(450),page(388),refs['exti_irqs'],refs['dma_exti_triggers']],
                         scope='package_route_only_not_electrical_or_board_qualification'))
    # Endpoint contracts are finite domains, not complete application recipes.
    profiles=[
        dict(id='gpio.exti.input',kind='interrupt_source',pins=[p['pin'] for p in pins],
             parameters={'gpio_mode':['input'],'edge':['rising','falling','both'],'cpu_delivery':['interrupt','event','both']},
             evidence=[page(451)],
             required_resources=['physical_pad','exti_port_selection','line_configuration_owner','NVIC_handler_if_interrupt'],
             sdk_steps=[{'action':'configure_GPIO_clock_and_input','evidence':[refs['gpio_init']]},
                        {'action':'HAL_EXTI_Init_then_SetConfig','evidence':[refs['exti_lifecycle'],refs['exti_config']]},
                        {'action':'register_callback_or_bind_weak_callback','applies_when':'interrupt_or_both_only',
                         'evidence':[refs['exti_types'],refs['exti_callback']]},
                        {'action':'set_pending_policy_and_NVIC_then_enable_delivery','evidence':[page(451),refs['exti_enable']],
                         'qualification':'author_integration_requirement_not_complete_source_prescribed_order'}],
             release_requirements=['mask_owned_CPU_delivery','synchronize_IRQ_and_callback','quiesce_all_mux_consumers_before_reassignment'],
             limits=['Event-only delivery does not latch pending flags under RM Table104.',
                     'One adapter owner per IRQ vector and line; no independent handles clearing the same pending state.'],
             unknown_gates=['board_electrical_and_reservations','exact_silicon_revision_errata','NVIC_priority_policy','race_safe_start_stop_adapter']),
        dict(id='gpio.idr_to_sram.triggered',kind='DMA_endpoint_pair',
             source={'class':'GPIO_IDR','ports':['A','B','C','D','E','H'],'addressing':'fixed','width_bytes':4},
             destination={'class':'authorized_SRAM_buffer','addressing':'increment','width_bytes':4},
             request={'kind':'software','SWREQ':1,'hardware_request':'ignored'},
             trigger={'selector_domain':list(range(8)),'source':'Table94_exti0_to_exti7',
                      'stage_relative_to_EXTI_edge_detector':'unresolved','polarity':['rising','falling'],'TRIGM':3},
             constraints={'block_bytes_multiple':4,'max_block_bytes':65532,'min_block_bytes':4,'source_alignment':4,'destination_alignment':4},
             evidence=[page(89),page(343),page(388),page(390),page(408),page(410),page(432),page(434),page(450),
                       refs['dma_exti_triggers'],refs['dma2_exti_triggers']],
             required_resources=['one_LPDMA_channel','exti_port_selection','source_GPIO_configuration_owner',
                                 'destination_buffer_exclusive_writer','GPIO_clock_held_while_active','LPDMA_clock_held_while_active',
                                 'completion_and_error_delivery_owner'],
             unknown_gates=['SDK_adapter_not_authored','SRAM_range_and_access_authorization','GPIO_port_clock','pin_input_configuration',
                            'trigger_rate_and_overrun_policy','exact_silicon_revision_errata','buffer_release_and_callback_synchronization',
                            'EXTI_trigger_stage_and_edge_configuration'],
             limits=['Reads a port register; pin mask filtering is separate software processing.',
                     'The register read occurs after trigger arbitration, not a synchronous pad latch at the edge.',
                     'Trigger overrun can lose samples without disabling the channel.',
                     'Direct single block only: LLR=0; completion clears EN. Later edges do not restart the channel.',
                     'Line mux and edge configuration are jointly owned until the trigger stage is resolved.',
                     'No claim of tested throughput or lossless sampling.'])]
    for profile in profiles:
        profile.update(review='candidate',implementation='not_authored',build='not_tested',hardware='not_observed')
    profiles.append(dict(id='timer.basic.fixed_clock',kind='periodic_update',instances=['TIM6','TIM7'],
        parameter_domain_rule='timer.basic.domain',formula='nominal_update_hz = HCLK / ((PSC+1)*(ARR+1))',
        formula_kind='derived_from_upcounter_and_prescaler_no_float_rounding',evidence=[page(271),page(388),page(389),page(1261),page(1262),page(1278),page(1279)],
        fixed_mode={'DITHEN':0,'OPM':0,'ADSYNC':0,'UDIS':0},
        required_resources=['TIM6_or_TIM7_RCC_clock_held_while_active_with_documented_enable_delay'],
        start_requirements=['Load buffered PSC/ARR with UG and apply pending/request policy before starting consumers.'],
        limits=['TIM6/7 TRGO is not a Table94 LPDMA trigger; these profiles do not form a direct hardware chain.'],
        unknown_gates=['race_safe_SDK_start_stop','clock_accuracy','exact_silicon_revision_errata','IRQ_DMA_consumer_ownership'],
        review='candidate',implementation='not_authored',build='not_tested',hardware='not_observed'))
    obj=dict(schema='bkpt.c5-composition-candidate/1',publication='unpublished',target=base['target'],
             base_candidate_sha256=sha((ROOT/'candidate.json').read_bytes()),sources=sources,evidence=evidence,
             rules=rules,gpio_exti_routes=pins,profiles=profiles,
             errata_screen={'evidence':[page(1,'ES0676'),page(2,'ES0676'),page(3,'ES0676')],
                            'scope':'Applicability and complete summary screened for LPDMA, EXTI, TIM6/7 headings.',
                            'result':'No dedicated headings; this does not establish absence of limitations.',
                            'remaining':'Core/system and application-dependent effects, silicon revision and workarounds remain gates.',
                            'GPIO_PC13':'ES2.2.2 applies to both listed revisions for LQFP crystal LSE with PC13 toggling; revision alone does not clear this condition.'},
             conflicts=[{'id':'trigger_masked_typo','evidence':[page(432)],'state':'conflicting_prose',
                         'detail':'TRIGSEL prose active for00 conflicts with TRIGPOL00 masked and TRIGM00/11 ignored; SDK comments corroborate rising01/falling10.'},
                        {'id':'comp2_trigger36','evidence':[refs['dma_trigger_scope'],page(389)],'state':'unresolved_source_divergence',
                         'detail':'SDK declares COMP2 trigger36; RM Table94 ends at35. Not admitted to finite trigger domain.'},
                        {'id':'timer_clock','evidence':[page(271),page(1261)],'state':'conflicting',
                         'detail':'Only equal HCLK/PCLK1 admitted for TIM6/7.'},
                        {'id':'timer_prescaler_range','evidence':[page(1259),page(1262),page(1278)],'state':'conflicting_prose',
                         'detail':'Feature summary says divisor 1..65535; functional/register model says PSC+1 up to65536. Candidate uses register model; unresolved prose remains explicit.'},
                        {'id':'TIM5_presence','evidence':[page(1134),page(20,'DS15135'),page(21,'DS15135'),refs['tim_instances']],
                         'state':'conflicting_presence','detail':'Family RM table lists TIM5; exact DS inventory and SDK predicates do not. Excluded from executable candidates.'}],
             full_device_coverage=False,operation_permission='none')
    if write_output:write('composition.json',obj)
    return obj


def check_dma(bundle, transfers):
    """Check one finite DMA group only; no CPU EXTI participants or runtime authorization."""
    rules={r['id']:r['value'] for r in bundle['rules']}
    channels,line_owners=set(),{}
    routes={r['pin']:r for r in bundle['gpio_exti_routes']}
    for transfer in transfers:
        keys={'controller','channel','source_width','destination_width','source_address','destination_address',
              'block_bytes','request','trigger'}
        if not isinstance(transfer,dict) or set(transfer)!=keys:return 'unknown_or_malformed_fields'
        ctrl,ch=transfer['controller'],transfer['channel']
        if not isinstance(ctrl,str) or type(ch)is not int:return 'malformed_channel'
        if ctrl not in rules['dma.channel.domain'] or ch not in rules['dma.channel.domain'][ctrl]:return 'outside_channel_domain'
        if (ctrl,ch) in channels:return 'channel_conflict'
        channels.add((ctrl,ch))
        sw,dw=transfer['source_width'],transfer['destination_width']
        if type(sw)is not int or type(dw)is not int or sw not in rules['dma.width'] or dw not in rules['dma.width']:return 'invalid_width'
        if sw!=4 or dw!=4:return 'outside_reviewed_endpoint_width'
        sa,da,n=transfer['source_address'],transfer['destination_address'],transfer['block_bytes']
        if any(type(v)is not int or v<0 for v in [sa,da,n]):return 'malformed_quantity'
        if any(v>0xffffffff for v in [sa,da]) or da+n>0x100000000 or sa+sw>0x100000000:return 'address_range_overflow'
        if sa%sw or da%dw:return 'misaligned_address'
        if not rules['dma.block']['min_nonzero']<=n<=rules['dma.block']['max_bytes'] or n%sw:return 'invalid_block'
        request=transfer['request']
        if not isinstance(request,dict):return 'malformed_request'
        if request=={'kind':'software'}:pass
        elif set(request)=={'kind','signal'} and request['kind']=='hardware' and isinstance(request['signal'],str) and request['signal']:
            return 'outside_reviewed_request_domain'
        else:return 'malformed_request'
        trigger=transfer['trigger']
        if trigger is not None:
            if not isinstance(trigger,dict) or set(trigger)!={'selector','polarity','granularity','pin','exti_edge'}:return 'malformed_trigger'
            if type(trigger['selector'])is not int or trigger['selector'] not in range(8):return 'outside_reviewed_trigger_domain'
            if trigger['polarity'] not in ('rising','falling'):return 'invalid_trigger_polarity'
            if not isinstance(trigger['granularity'],str) or trigger['granularity']!='single':return 'outside_reviewed_trigger_granularity'
            if trigger['exti_edge'] not in ('rising','falling','both'):return 'invalid_EXTI_edge'
            pin=trigger['pin']
            if not isinstance(pin,str) or pin not in routes:return 'outside_package_route_domain'
            route=routes[pin]
            if route['dma_trigger_selector']!=trigger['selector']:return 'pin_trigger_mismatch'
            if route['context_constraints']:return 'device_function_context_required'
            # Explicit shared EXTI configuration, distinct from each DMA channel's TRIGPOL.
            owner=(route['port'],trigger['exti_edge'])
            if trigger['selector'] in line_owners and line_owners[trigger['selector']]!=owner:return 'exti_line_configuration_conflict'
            line_owners[trigger['selector']]=owner
    return 'abstract_constraints_only_endpoint_and_runtime_gates_unknown'


def verify(bundle):
    # Full comparison binds safety prose, references, domains and unresolved states to reviewed authoring.
    # This is consistency validation, not independent proof of the author's semantic correctness.
    assert bundle==build(write_output=False)
    assert bundle['base_candidate_sha256']==sha((ROOT/'candidate.json').read_bytes())
    base=json.loads((ROOT/'candidate.json').read_text(encoding='utf-8'))
    assert bundle['target']==base['target']
    assert bundle['full_device_coverage'] is False
    for source in bundle['sources'].values():
        assert sha(Path(source['path']).read_bytes())==source['sha256']
        if 'extraction_path'in source:
            assert sha(Path(source['extraction_path']).read_bytes())==source['extraction_sha256']
    parsed={}
    for key,e in bundle['evidence'].items():
        source=bundle['sources'][e['source']]
        if 'pointer'in e:
            value=json.loads(Path(source['path']).read_text(encoding='utf-8'))
            for part in e['pointer'].strip('/').split('/'):
                value=value[int(part)] if isinstance(value,list) else value[part]
            assert value==e['value']
            text=json.dumps(value,sort_keys=True,separators=(',',':'))
        elif 'page'in e:
            assert key==f"{e['source']}:p{e['page']}"
            path=source['extraction_path']
            if path not in parsed:parsed[path]=json.loads(Path(path).read_text(encoding='utf-8'))
            raw=parsed[path]
            assert raw['document_sha256']==source['sha256']
            text=next(p['text'] for p in raw['pages'] if p['page']==e['page'])
        else:
            text='\n'.join(Path(source['path']).read_text(encoding='utf-8').splitlines()[e['start_line']-1:e['end_line']])
        assert text==e['text'] and sha(text.encode())==e['text_sha256']
    for item in bundle['rules']+bundle['profiles']:
        assert item['review']=='candidate' and item['hardware']=='not_observed'
    for item in bundle['rules']:
        assert item['implementation']=='not_integrated' and item['errata_coverage']=='incomplete'
    rules={r['id']:r['value'] for r in bundle['rules']}
    assert set(rules)=={'dma.channel.domain','dma.width','dma.alignment','dma.block','dma.request.exclusion',
        'dma.trigger.broadcast','dma.trigger.overrun','dma.trigger.granularity','dma.trigger.polarity','dma.abort',
        'dma.bus','gpio.input.register','gpio.software_dma','exti.mux','exti.delivery','exti.pending',
        'exti.callback','exti.release','exti.callback.order','gpio.clock','timer.kernel.models',
        'timer.basic.domain','timer.prescaler.load','timer.UG.side_effect','timer.trigger.clock'}
    assert rules['dma.channel.domain']=={'LPDMA1':[0,1,2,3],'LPDMA2':[0,1,2,3]}
    assert rules['dma.width']==[1,2,4]
    assert rules['dma.block']=={'min_nonzero':1,'max_bytes':65535,'multiple_of':'source_width'}
    assert rules['dma.trigger.granularity']=={'block':0,'link':2,'single':3}
    assert rules['timer.basic.domain']=={'instances':['TIM6','TIM7'],'hclk_hz':144000000,'pclk1_hz':144000000,
        'psc_min':0,'psc_max':65535,'arr_min':1,'arr_max':65535,'DITHEN':0}
    dma_profile=next(p for p in bundle['profiles'] if p['id']=='gpio.idr_to_sram.triggered')
    assert dma_profile['trigger']=={'selector_domain':list(range(8)),'source':'Table94_exti0_to_exti7',
        'stage_relative_to_EXTI_edge_detector':'unresolved','polarity':['rising','falling'],'TRIGM':3}
    gates={
        'gpio.exti.input':{'board_electrical_and_reservations','exact_silicon_revision_errata','NVIC_priority_policy','race_safe_start_stop_adapter'},
        'gpio.idr_to_sram.triggered':{'SDK_adapter_not_authored','SRAM_range_and_access_authorization','GPIO_port_clock','pin_input_configuration',
                            'trigger_rate_and_overrun_policy','exact_silicon_revision_errata','buffer_release_and_callback_synchronization',
                            'EXTI_trigger_stage_and_edge_configuration'},
        'timer.basic.fixed_clock':{'race_safe_SDK_start_stop','clock_accuracy','exact_silicon_revision_errata','IRQ_DMA_consumer_ownership'}}
    assert {p['id'] for p in bundle['profiles']}==set(gates)
    for profile in bundle['profiles']:
        assert profile['implementation']=='not_authored' and profile['build']=='not_tested'
        assert set(profile['unknown_gates'])==gates[profile['id']]
    for category in ['rules','profiles']:
        assert len({v['id'] for v in bundle[category]})==len(bundle[category])
    assert {c['id'] for c in bundle['conflicts']}=={'trigger_masked_typo','comp2_trigger36','timer_clock','timer_prescaler_range','TIM5_presence'}
    def references(value):
        if isinstance(value,dict):
            for key,child in value.items():
                if key=='evidence' and isinstance(child,list):
                    assert all(ref=='base:package_pads' or ref in bundle['evidence'] for ref in child)
                else:references(child)
        elif isinstance(value,list):
            for child in value:references(child)
    references(bundle)
    for rule in bundle['rules']:
        assert rule['evidence'] and all(e in bundle['evidence'] for e in rule['evidence'])
    assert len(bundle['gpio_exti_routes'])==52
    expected={p['signal']:p for p in base['package_pads'] if re.fullmatch(r'P[A-H]\d+',p['signal'])}
    assert {p['pin'] for p in bundle['gpio_exti_routes']}==set(expected)
    for route in bundle['gpio_exti_routes']:
        original=expected[route['pin']]
        m=re.fullmatch(r'P([A-H])(\d+)',original['signal']);line=int(m[2])
        irq=next(i for i in base['irq_declarations'] if i['symbol']==f'EXTI{line}_IRQn')
        assert (route['pad'],route['port'],route['exti_line'],route['irq'],route['irq_symbol'])==(original['pad'],m[1],line,irq['number'],irq['symbol'])
        assert route['dma_trigger_selector']==(line if line<8 else None)
        assert route['context_constraints']==original['device_function_constraints']
    assert bundle['publication']=='unpublished' and bundle['operation_permission']=='none'


def timer_rate(bundle,instance,hclk,pclk1,psc,arr):
    domain=next(r['value'] for r in bundle['rules'] if r['id']=='timer.basic.domain')
    if not isinstance(instance,str) or instance not in domain['instances']:return {'status':'outside_instance_domain'}
    if any(type(v)is not int for v in [hclk,pclk1,psc,arr]):return {'status':'malformed_quantity'}
    if hclk!=pclk1:return {'status':'conflicting_clock_domain'}
    if hclk!=domain['hclk_hz']:return {'status':'outside_fixed_foundation'}
    if not domain['psc_min']<=psc<=domain['psc_max'] or not domain['arr_min']<=arr<=domain['arr_max']:return {'status':'outside_counter_domain'}
    frequency=Fraction(hclk,(psc+1)*(arr+1))
    return dict(status='derived_nominal_rate_only',numerator=str(frequency.numerator),denominator=str(frequency.denominator),
                physical_clock_accuracy='unknown',implementation='not_authored')


def acceptance(bundle):
    verify(bundle)
    base=dict(controller='LPDMA1',channel=0,source_width=4,destination_width=4,
              source_address=0x1000,destination_address=0x2000,block_bytes=64,
              request={'kind':'software'},trigger={'selector':0,'polarity':'rising','granularity':'single','pin':'PA0','exti_edge':'both'})
    cases=[]
    def test(name,transfers,expected):
        actual=check_dma(bundle,transfers);assert actual==expected,(name,actual,expected)
        cases.append(dict(id=name,expected=expected,observed=actual))
    ok='abstract_constraints_only_endpoint_and_runtime_gates_unknown'
    test('well_formed_is_not_executable',[base],ok)
    test('broadcast_trigger',[base,dict(base,controller='LPDMA2',destination_address=0x2100)],ok)
    test('different_port_same_line',[base,dict(base,controller='LPDMA2',trigger=dict(base['trigger'],pin='PB0'))],
         'exti_line_configuration_conflict')
    test('different_edge_same_line',[base,dict(base,controller='LPDMA2',trigger=dict(base['trigger'],exti_edge='falling'))],
         'exti_line_configuration_conflict')
    test('different_DMA_polarity_same_EXTI_config',[base,dict(base,controller='LPDMA2',destination_address=0x2100,
         trigger=dict(base['trigger'],polarity='falling'))],ok)
    test('restricted_pin_context',[dict(base,trigger=dict(base['trigger'],pin='PH0'))],'device_function_context_required')
    test('unhashable_granularity',[dict(base,trigger=dict(base['trigger'],granularity=[]))],'outside_reviewed_trigger_granularity')
    test('linked_trigger_outside_profile',[dict(base,trigger=dict(base['trigger'],granularity='link'))],'outside_reviewed_trigger_granularity')
    test('mixed_widths_outside_profile',[dict(base,destination_width=2)],'outside_reviewed_endpoint_width')
    test('address_overflow',[dict(base,destination_address=0xfffffff0)],'address_range_overflow')
    test('unbonded_trigger_pin',[dict(base,trigger=dict(base['trigger'],pin='PB11'))],'outside_package_route_domain')
    test('trigger_pin_mismatch',[dict(base,trigger=dict(base['trigger'],pin='PA1'))],'pin_trigger_mismatch')
    test('same_channel',[base,base],'channel_conflict')
    a=dict(base,request={'kind':'hardware','signal':'spi1_rx_dma'})
    test('unreviewed_hardware_request',[a],'outside_reviewed_request_domain')
    test('request_alias_rejected',[dict(a,request={'kind':'hardware','signal':'SPI1_RX_DMA'})],'outside_reviewed_request_domain')
    for name,patch,expected in [
        ('family_channel',{'channel':7},'outside_channel_domain'),
        ('boolean_channel',{'channel':True},'malformed_channel'),
        ('width8',{'source_width':8},'invalid_width'),
        ('source_alignment',{'source_address':0x1001},'misaligned_address'),
        ('destination_alignment',{'destination_address':0x2002},'misaligned_address'),
        ('block_multiple',{'block_bytes':3},'invalid_block'),
        ('block_overflow',{'block_bytes':65536},'invalid_block'),
        ('empty_block',{'block_bytes':0},'invalid_block'),
        ('unexpected_option',{'PFREQ':1},'unknown_or_malformed_fields'),
        ('unknown_trigger',{'trigger':dict(base['trigger'],selector=36)},'outside_reviewed_trigger_domain'),
        ('both_edges_dma',{'trigger':dict(base['trigger'],polarity='both')},'invalid_trigger_polarity')]:
        test(name,[dict(base,**patch)],expected)
    for name,args,expected in [
        ('timer_1khz',('TIM6',144000000,144000000,143,999),'derived_nominal_rate_only'),
        ('timer_unequal_clocks',('TIM6',144000000,72000000,143,999),'conflicting_clock_domain'),
        ('timer_zero_ARR',('TIM7',144000000,144000000,143,0),'outside_counter_domain'),
        ('timer_16bit_bound',('TIM7',144000000,144000000,65536,100),'outside_counter_domain'),
        ('timer_TIM5_leak',('TIM5',144000000,144000000,143,999),'outside_instance_domain'),
        ('timer_other_foundation',('TIM6',72000000,72000000,71,999),'outside_fixed_foundation')]:
        actual=timer_rate(bundle,*args)
        assert actual['status']==expected
        if name=='timer_1khz':assert (actual['numerator'],actual['denominator'])==('1000','1')
        cases.append(dict(id=name,expected=expected,observed=actual))
    for name,mutate in [
        ('dropped_safety_limits',lambda b:b['profiles'][1].update(limits=[])),
        ('dropped_errata_screen',lambda b:b.pop('errata_screen')),
        ('weakened_conflict',lambda b:b['conflicts'][0].update(state='resolved')),
        ('widened_DMA_channels',lambda b:next(r for r in b['rules'] if r['id']=='dma.channel.domain')['value']['LPDMA1'].append(4)),
        ('widened_timer_instances',lambda b:next(r for r in b['rules'] if r['id']=='timer.basic.domain')['value']['instances'].append('TIM5')),
        ('missing_release_rule',lambda b:b.update(rules=[r for r in b['rules'] if r['id']!='exti.release'])),
        ('false_trigger_stage',lambda b:b['profiles'][1]['trigger'].update(stage_relative_to_EXTI_edge_detector='verified_raw_mux')),
        ('unbonded_route',lambda b:b['gpio_exti_routes'][0].update(pin='PB11',exti_line=99)),
        ('target_widening',lambda b:b['target'].update(device='STM32C59xxx')),
        ('false_implementation',lambda b:b['profiles'][0].update(implementation='implemented')),
        ('false_build',lambda b:b['profiles'][0].update(build='tested')),
        ('false_full_coverage',lambda b:b.update(full_device_coverage=True)),
        ('dropped_gates',lambda b:b['profiles'][0].update(unknown_gates=[])),
        ('dropped_conflict',lambda b:b.update(conflicts=[])),
        ('forged_sdk_reference',lambda b:b['profiles'][0]['sdk_steps'][0].update(evidence=['fabricated']))]:
        changed=copy.deepcopy(bundle);mutate(changed)
        try:verify(changed)
        except (AssertionError,KeyError):cases.append(dict(id=name,observed='rejected'))
        else:raise AssertionError(name)
    write('composition-acceptance.json',dict(bundle_sha256=sha((ROOT/'composition.json').read_bytes()),
          author_validator_sha256=sha(Path(__file__).read_bytes()),passed=len(cases),fixtures=cases,
          fixture_addresses='Synthetic aligned integers only; no physical endpoint or SRAM mapping is asserted.',
          qualification='Host constraint/evidence checks only; no implementation/build/hardware qualification.'))


if __name__=='__main__':
    result=build();acceptance(result)
    print(json.dumps({'rules':len(result['rules']),'routes':len(result['gpio_exti_routes']),
                      'profiles':len(result['profiles']),'sha256':sha((ROOT/'composition.json').read_bytes())}))
