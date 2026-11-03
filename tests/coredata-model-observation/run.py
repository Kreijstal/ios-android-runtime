#!/usr/bin/env python3
"""Run only freshly authored public API cases, keeping all child streams private."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import shutil
import subprocess
import sys

import schema

REPOSITORY = 'Kreijstal/ios-android-runtime'
REF = 'refs/heads/test/coredata-model-observation-20261103'
ROOT = Path(__file__).resolve().parent
class Failed(Exception): pass

def require(value):
    if not value: raise Failed()

def invoke(command, *, identity=False):
    # Only narrowly validated version/control output is captured. Build/native output
    # goes directly to DEVNULL before process creation and is never stored.
    with subprocess.Popen(command, stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE if identity else subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL) as process:
        if identity:
            data=process.stdout.read(4097)
            require(len(data) <= 4096)
            require(process.wait() == 0)
            return data.decode('ascii')
        return process.wait()

def version(command, pattern):
    text=invoke(command, identity=True).strip()
    match=re.fullmatch(pattern, text)
    require(match is not None)
    return match.group(1)

def read_json(path, validator, maximum=32768):
    require(path.is_file() and not path.is_symlink() and path.stat().st_size <= maximum)
    def unique(pairs):
        result={}
        for key, value in pairs:
            require(key not in result); result[key]=value
        return result
    body=json.loads(path.read_text(), object_pairs_hook=unique,
                    parse_constant=lambda _: (_ for _ in ()).throw(Failed()))
    return validator(body)

def build(source, work, target, sdk):
    out=work/target
    code=invoke(['xcodebuild', '-project', str(source/'Observation.xcodeproj'),
                 '-target', target, '-configuration', 'Release', '-sdk', sdk,
                 'SYMROOT='+str(out/'products'), 'OBJROOT='+str(out/'objects'),
                 'CODE_SIGNING_ALLOWED=NO', 'CODE_SIGNING_REQUIRED=NO',
                 'COMPILER_INDEX_STORE_ENABLE=NO'])
    require(code == 0)
    return out/'products'/('Release-iphoneos' if sdk == 'iphoneos' else 'Release')/(target+'.app')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--harness-sha', required=True)
    parser.add_argument('--work', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args=parser.parse_args()
    # Fresh owned work only; no cached resources/models or existing outputs.
    require(not args.work.exists() and not args.output.exists())
    args.work.mkdir(parents=True)
    resource.setrlimit(resource.RLIMIT_CORE, (0,0))
    source={'repository':os.environ.get('GITHUB_REPOSITORY'), 'ref':os.environ.get('GITHUB_REF'),
            'event':os.environ.get('GITHUB_EVENT_NAME'), 'harnessSha':args.harness_sha,
            'eventSha':os.environ.get('GITHUB_SHA')}
    require(source['repository'] == REPOSITORY and source['ref'] == REF and source['event'] == 'push')
    require(all(type(source[key]) is str and re.fullmatch('[a-f0-9]{40}', source[key]) for key in ('harnessSha','eventSha')))
    checked=version(['git','-C',str(ROOT),'rev-parse','HEAD'], r'([a-f0-9]{40})')
    require(checked == args.harness_sha)
    # No tracked changes to the source pinned by workflow; untracked build data lives outside.
    require(invoke(['git','-C',str(ROOT),'diff','--quiet','HEAD','--']) == 0)
    report={'format':1,'result':'failed','stage':'guard','source':source,'tools':{},
            'models':None,'compiled':None,'environment':{},
            'controlSources':{'required':None,'optional':None},'concordance':{'mac':False,'ios':False}}
    try:
        report['stage']='identity'
        report['tools']={
            'macOS':version(['sw_vers','-productVersion'],r'(\d+(?:\.\d+){1,2})'),
            'xcode':version(['xcodebuild','-version'],r'(Xcode \d+(?:\.\d+){0,2}\nBuild version [A-Za-z0-9]+)').replace('\n',' '),
            'macSDK':version(['xcrun','--sdk','macosx','--show-sdk-version'],r'(\d+(?:\.\d+){0,2})'),
            'iosSDK':version(['xcrun','--sdk','iphoneos','--show-sdk-version'],r'(\d+(?:\.\d+){0,2})'),
            'architecture':version(['uname','-m'],r'(arm64|x86_64)')}
        report['stage']='build-broad'
        broad=build(ROOT,args.work/'broad','MacObservation','macosx')
        ios=build(ROOT,args.work/'broad','IOSModel','iphoneos')
        environment=build(ROOT,args.work/'broad','MacEnvironment','macosx')
        report['stage']='build-controls'
        resources={'mac':[],'ios':[]}
        template=(ROOT/'Models/Probe.xcdatamodel/contents').read_bytes()
        require(template.count(b'optional="NO"') == 1)
        for name, optional in [('A',False),('B',True),('A2',False),('B2',True)]:
            # Same source basename, entity/attribute/type, project/target/product settings.
            # Distinct fresh source/derived dirs change only optionality within each target.
            case=args.work/'controls'/name
            shutil.copytree(ROOT,case/'source',ignore=shutil.ignore_patterns('__pycache__'))
            data=template.replace(b'optional="NO"',b'optional="YES"') if optional else template
            (case/'source/Models/Probe.xcdatamodel/contents').write_bytes(data)
            report['controlSources']['optional' if optional else 'required']=hashlib.sha256(data).hexdigest()
            for platform,target,sdk in [('mac','ControlMac','macosx'),('ios','ControlIOS','iphoneos')]:
                product=build(case/'source',case/'build',target,sdk)
                resources[platform].append(product/('Probe.mom' if platform=='ios' else 'Contents/Resources/Probe.mom'))
        executable=broad/'Contents/MacOS/MacObservation'
        report['stage']='model-api'
        native_path=args.work/'model.json'
        code=invoke([str(executable),str(broad/'Contents/Resources/Observation.mom'),str(ios/'Observation.mom'),str(native_path),str(args.work/'broad-data')])
        report['models']=read_json(native_path,schema.model)
        require(code == 0 and report['models']['result'] == 'pass')
        report['stage']='compiled-api'
        compiled_path=args.work/'compiled.json'
        code=invoke([str(executable),'--compiled-controls',str(compiled_path),str(args.work/'control-data'),*[str(path) for platform in ('mac','ios') for path in resources[platform]]])
        report['compiled']=read_json(compiled_path,schema.compiled)
        require(code == 0 and report['compiled']['result'] == 'pass')
        detached=report['models']['archiveFacts'][0]
        for platform in ('mac','ios'):
            fact=report['compiled']['platforms'][platform]
            report['concordance'][platform]=detached['status']==fact['status']=='observed' and all(detached[key]==fact[key] for key in ('field','required','optional'))
        report['stage']='environment-api'
        for mode in schema.MODES:
            path=args.work/('environment-'+mode+'.json')
            code=invoke([str(environment/'Contents/MacOS/MacEnvironment'),mode,str(path)])
            record=read_json(path,lambda body:schema.environment(body,mode),4096) if path.exists() else None
            report['environment'][mode]={'exitCategory':'success' if code==0 else ('signal' if code<0 else 'failure'),'observation':record}
        report['stage']='complete'; report['result']='pass'
    except Exception:
        # Exceptions, diagnostics, paths and native values never become failure text.
        report['result']='failed'
    schema.final(report)
    data=json.dumps(report,sort_keys=True,separators=(',',':')).encode()
    require(len(data)<=65536)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_bytes(data+b'\n')
    print('SAFE_REPORT_VALIDATED')
    return 0 if report['result']=='pass' else 1

if __name__ == '__main__':
    try: raise SystemExit(main())
    except Exception: print('FIXED_PROPOSAL_FAILURE'); raise SystemExit(2)
