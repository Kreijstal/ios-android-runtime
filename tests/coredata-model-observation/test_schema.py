import copy
import unittest
import schema

class ClosedSchemaTests(unittest.TestCase):
    def body(self):
        return {'format': 1, 'result': 'failed', 'failure': 'api-failure',
                'stage': 'start', 'scope': copy.deepcopy(schema.SCOPE),
                'checks': {key: False for key in schema.CHECKS}, 'archiveFacts': [],
                'modelHashes': {'mac': None, 'ios': None}}

    def test_fixed_failure_allowed(self):
        schema.model(self.body())

    def test_unexpected_error_text_rejected(self):
        value = self.body(); value['exceptionDescription'] = 'never upload'
        with self.assertRaises(schema.Rejected): schema.model(value)

    def test_success_cannot_hide_unrun_checks(self):
        value = self.body(); value.update(result='pass', failure='none', stage='complete')
        with self.assertRaises(schema.Rejected): schema.model(value)

    def test_field_identifier_and_primitive_bounds(self):
        value = self.body()
        fact = {'attributeType': 'String', 'status': 'observed',
                'requiredArchiveSha256': 'a'*64, 'optionalArchiveSha256': 'b'*64,
                'field': 'authoredCandidate', 'required': {'present': False},
                'optional': {'present': True, 'kind': 'boolean', 'value': True}}
        value['archiveFacts'] = [fact]; value['checks']['archiveObservationControls']=True; schema.model(value)
        fact['field'] = 'unexpected\ntext'
        with self.assertRaises(schema.Rejected): schema.model(value)
        fact['field'] = 'authoredCandidate'; fact['optional'] = {'present': True, 'kind': 'integer', 'value': 2**40}
        with self.assertRaises(schema.Rejected): schema.model(value)

    def test_detached_fact_requires_public_controls(self):
        value=self.body()
        value['archiveFacts']=[{'attributeType':'String','status':'observed','requiredArchiveSha256':'a'*64,
                               'optionalArchiveSha256':'b'*64,'field':'knownCandidate',
                               'required':{'present':False},'optional':{'present':True,'kind':'integer','value':1}}]
        with self.assertRaises(schema.Rejected): schema.model(value)

    def test_boolean_is_not_integer(self):
        with self.assertRaises(schema.Rejected): schema.scalar_state({'present': True, 'kind': 'integer', 'value': True})

    def test_failed_default_does_not_invent_facts(self):
        schema.environment({'format': 1, 'mode': 'default', 'result': 'entered', 'facts': None}, 'default')
        with self.assertRaises(schema.Rejected):
            schema.environment({'format': 1, 'mode': 'default', 'result': 'entered', 'facts': {'coordinatorPresent': False}}, 'default')

    def test_generic_success_requires_provided_identity(self):
        with self.assertRaises(schema.Rejected):
            schema.environment({'format': 1, 'mode': 'generic-fresh', 'result': 'pass', 'facts': {'providedIdentity': False}}, 'generic-fresh')


class CompiledSchemaTests(unittest.TestCase):
    def body(self):
        fact={'status':'inconclusive','field':None,'required':None,'optional':None,'outputHashes':['a'*64]*4}
        return {'format':1,'result':'pass','failure':'none','stage':'complete','scope':schema.COMPILED_SCOPE,
                'controls':True,'platforms':{'mac':copy.deepcopy(fact),'ios':copy.deepcopy(fact)}}

    def test_behavior_pass_does_not_require_encoding_discovery(self):
        schema.compiled(self.body())

    def test_encoding_fact_requires_public_controls(self):
        value=self.body(); value['controls']=False
        fact=value['platforms']['mac']; fact.update(status='observed',field='knownCandidate',required={'present':False},optional={'present':True,'kind':'integer','value':2})
        with self.assertRaises(schema.Rejected): schema.compiled(value)

    def test_equal_states_are_not_discovery(self):
        value=self.body(); fact=value['platforms']['mac']
        fact.update(status='observed',field='knownCandidate',required={'present':False},optional={'present':False})
        with self.assertRaises(schema.Rejected): schema.compiled(value)

    def test_unknown_native_field_rejected(self):
        value=self.body(); value['platforms']['mac']['description']='forbidden output'
        with self.assertRaises(schema.Rejected): schema.compiled(value)

if __name__ == '__main__': unittest.main()
