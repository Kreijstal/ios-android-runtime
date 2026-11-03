"""Closed result schemas; never return unvalidated native content for upload."""
import re

TYPES = ['String', 'Date', 'URI', 'Boolean']
CHECKS = {'programmaticGetters', 'programmaticRoundtrip', 'archiveObservationControls',
          'programmaticNilValidation', 'macCompiledGetters', 'macCompiledNilValidation',
          'iosCompiledGetters', 'iosCompiledNilValidation'}
STAGES = {'start', 'resource.inputs', 'programmatic.getters', 'programmatic.archive',
          'archive.controls', 'programmatic.validation', 'mac.load', 'mac.getters',
          'mac.validation', 'ios.load', 'ios.getters', 'ios.validation', 'complete'}
MODES = ['default', 'direct', 'generic-fresh', 'generic-seeded']
SCOPE = {'requiredNilNegative': 'String', 'optionalPersistence': TYPES,
         'compiledResources': 'public-load-and-hash-only',
         'archiveFields': 'detached-authored-attribute-buffers-only'}

class Rejected(ValueError):
    pass

def require(condition):
    if not condition:
        raise Rejected('CLOSED_SCHEMA_REJECTED')

def keys(value, expected):
    require(type(value) is dict and set(value) == set(expected))

def sha(value):
    require(type(value) is str and re.fullmatch('[a-f0-9]{64}', value) is not None)

def scalar_state(value):
    require(type(value) is dict and type(value.get('present')) is bool)
    if not value['present']:
        keys(value, {'present'})
    else:
        keys(value, {'present', 'kind', 'value'})
        if value['kind'] == 'boolean':
            require(type(value['value']) is bool)
        elif value['kind'] == 'integer':
            require(type(value['value']) is int and -(2**31) <= value['value'] < 2**31)
        else:
            require(False)

def model(value):
    keys(value, {'format', 'result', 'failure', 'stage', 'scope', 'checks', 'archiveFacts', 'modelHashes'})
    require(type(value['format']) is int and value['format'] == 1)
    require(value['result'] in {'pass', 'failed', 'incomplete'} and value['stage'] in STAGES)
    require(value['failure'] in {'none', 'api-failure', 'exception', 'ios-resource-rejected', 'resource-missing'})
    require(value['scope'] == SCOPE)
    keys(value['checks'], CHECKS)
    require(all(type(item) is bool for item in value['checks'].values()))
    keys(value['modelHashes'], {'mac', 'ios'})
    for item in value['modelHashes'].values():
        if item is not None:
            sha(item)
    facts = value['archiveFacts']
    require(type(facts) is list and len(facts) <= 4)
    for index, fact in enumerate(facts):
        keys(fact, {'attributeType', 'status', 'requiredArchiveSha256', 'optionalArchiveSha256', 'field', 'required', 'optional'})
        require(fact['attributeType'] == TYPES[index])
        require(fact['status'] in {'observed', 'inconclusive', 'unparseable'})
        sha(fact['requiredArchiveSha256']); sha(fact['optionalArchiveSha256'])
        if fact['status'] == 'observed':
            require(value['checks']['archiveObservationControls'])
            require(type(fact['field']) is str and re.fullmatch('[A-Za-z_][A-Za-z0-9_]{0,63}', fact['field']) is not None)
            scalar_state(fact['required']); scalar_state(fact['optional'])
            require(fact['required'] != fact['optional'])
        else:
            require(fact['field'] is None and fact['required'] is None and fact['optional'] is None)
    if value['result'] == 'pass':
        require(value['stage'] == 'complete' and value['failure'] == 'none' and all(value['checks'].values()) and len(facts) == 4)
        require(all(item is not None for item in value['modelHashes'].values()))
    elif value['result'] == 'incomplete':
        require(value['stage'] == 'ios.load' and value['failure'] == 'ios-resource-rejected' and not value['checks']['iosCompiledGetters'])
    else:
        require(value['failure'] not in {'none', 'ios-resource-rejected'})
    return value

def environment(value, expected_mode):
    keys(value, {'format', 'mode', 'result', 'facts'})
    require(type(value['format']) is int and value['format'] == 1 and value['mode'] == expected_mode and expected_mode in MODES)
    require(value['result'] in {'entered', 'pass', 'failed'})
    facts = value['facts']
    if value['result'] == 'entered':
        require(facts is None)
        return value
    if expected_mode == 'default':
        keys(facts, {'concurrency', 'coordinatorPresent', 'parentPresent', 'repeatedIdentity'})
        require(facts['concurrency'] in {'confinement', 'private', 'main', 'unknown'})
        require(all(type(facts[key]) is bool for key in ('coordinatorPresent', 'parentPresent', 'repeatedIdentity')))
    elif expected_mode == 'generic-seeded':
        keys(facts, {'seedIdentity', 'providedIdentity'})
        require(all(type(item) is bool for item in facts.values()))
    else:
        keys(facts, {'providedIdentity'})
        require(type(facts['providedIdentity']) is bool)
    if value['result'] == 'pass' and expected_mode != 'default':
        require(all(facts.values()))
    return value

COMPILED_SCOPE = 'fresh-single-string-attribute-compiled-buffers-only'

def compiled(value):
    keys(value, {'format', 'result', 'failure', 'stage', 'scope', 'controls', 'platforms'})
    require(type(value['format']) is int and value['format'] == 1)
    require(value['result'] in {'pass', 'failed'} and value['failure'] in {'none', 'api-failure', 'control-failure'})
    require(value['stage'] in {'inputs', 'public-controls', 'bounded-observation', 'complete'})
    require(value['scope'] == COMPILED_SCOPE and type(value['controls']) is bool)
    require(type(value['platforms']) is dict and set(value['platforms']) <= {'mac', 'ios'})
    for fact in value['platforms'].values():
        keys(fact, {'status', 'field', 'required', 'optional', 'outputHashes'})
        require(fact['status'] in {'observed', 'inconclusive', 'unparseable'})
        require(type(fact['outputHashes']) is list and len(fact['outputHashes']) == 4)
        for item in fact['outputHashes']: sha(item)
        if fact['status'] == 'observed':
            require(value['controls'] and type(fact['field']) is str and re.fullmatch('[A-Za-z_][A-Za-z0-9_]{0,63}', fact['field']) is not None)
            scalar_state(fact['required']); scalar_state(fact['optional'])
            require(fact['required'] != fact['optional'])
        else:
            require(fact['field'] is None and fact['required'] is None and fact['optional'] is None)
    if value['result'] == 'pass':
        require(value['failure'] == 'none' and value['stage'] == 'complete' and value['controls'] and set(value['platforms']) == {'mac', 'ios'})
    else: require(value['failure'] != 'none')
    return value

def final(value):
    keys(value, {'format', 'result', 'stage', 'source', 'tools', 'models', 'compiled', 'environment', 'controlSources', 'concordance'})
    require(type(value['format']) is int and value['format'] == 1)
    require(value['result'] in {'pass', 'failed'})
    require(value['stage'] in {'guard', 'identity', 'build-broad', 'build-controls', 'model-api', 'compiled-api', 'environment-api', 'complete'})
    keys(value['source'], {'repository', 'ref', 'event', 'harnessSha', 'eventSha'})
    require(value['source']['repository'] == 'Kreijstal/ios-android-runtime')
    require(value['source']['ref'] == 'refs/heads/test/coredata-model-observation-20261103' and value['source']['event'] == 'push')
    for key in ('harnessSha', 'eventSha'): require(type(value['source'][key]) is str and re.fullmatch('[a-f0-9]{40}', value['source'][key]) is not None)
    require(value['source']['harnessSha'] == value['source']['eventSha'])
    require(type(value['tools']) is dict and set(value['tools']) <= {'macOS', 'xcode', 'macSDK', 'iosSDK', 'architecture'})
    for key, item in value['tools'].items():
        require(type(item) is str and re.fullmatch('[A-Za-z0-9 ._-]{1,80}', item) is not None)
    if value['models'] is not None: model(value['models'])
    if value['compiled'] is not None: compiled(value['compiled'])
    require(type(value['environment']) is dict and set(value['environment']) <= set(MODES))
    for mode, item in value['environment'].items():
        keys(item, {'exitCategory', 'observation'})
        require(item['exitCategory'] in {'success', 'failure', 'signal'})
        if item['observation'] is not None: environment(item['observation'], mode)
        if item['exitCategory'] == 'success': require(item['observation'] is not None and item['observation']['result'] == 'pass')
    keys(value['controlSources'], {'required', 'optional'})
    for item in value['controlSources'].values():
        if item is not None: sha(item)
    keys(value['concordance'], {'mac', 'ios'})
    for platform, agreed in value['concordance'].items():
        require(type(agreed) is bool)
        if agreed:
            require(value['models'] is not None and value['compiled'] is not None and len(value['models']['archiveFacts']) >= 1)
            left=value['models']['archiveFacts'][0]; right=value['compiled']['platforms'].get(platform)
            require(right is not None and left['status'] == right['status'] == 'observed')
            require(all(left[key] == right[key] for key in ('field', 'required', 'optional')))
    if value['result'] == 'pass':
        require(value['stage'] == 'complete' and set(value['tools']) == {'macOS', 'xcode', 'macSDK', 'iosSDK', 'architecture'})
        require(value['models'] is not None and value['models']['result'] == 'pass' and value['compiled'] is not None and value['compiled']['result'] == 'pass')
        require(set(value['environment']) == set(MODES) and all(item is not None for item in value['controlSources'].values()))
    return value
