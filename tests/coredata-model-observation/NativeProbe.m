#import <Foundation/Foundation.h>
#import <CoreData/CoreData.h>
#import <CommonCrypto/CommonDigest.h>
#import <CoreFoundation/CoreFoundation.h>
#include <limits.h>
#include <string.h>

static NSString *Stage = @"start";
static NSArray *TypeNames(void) { return @[@"String",@"Date",@"URI",@"Boolean"]; }
static NSArray *EntityNames(void) { return @[@"StringCase",@"DateCase",@"URICase",@"BooleanCase"]; }
static NSAttributeType TypeAt(NSUInteger index) {
    const NSAttributeType types[]={NSStringAttributeType,NSDateAttributeType,NSURIAttributeType,NSBooleanAttributeType};
    return types[index];
}
static NSString *Digest(NSData *data) {
    if (!data || data.length>16*1024*1024) return nil;
    unsigned char bytes[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes,(CC_LONG)data.length,bytes);
    NSMutableString *result=[NSMutableString stringWithCapacity:64];
    for (NSUInteger index=0;index<sizeof(bytes);index++) [result appendFormat:@"%02x",bytes[index]];
    return result;
}
static NSAttributeDescription *Attribute(NSString *name,NSAttributeType type,BOOL optional) {
    NSAttributeDescription *attribute=[NSAttributeDescription new];
    attribute.name=name; attribute.attributeType=type; attribute.optional=optional;
    return [attribute autorelease];
}
static NSManagedObjectModel *Model(void) {
    NSMutableArray *entities=[NSMutableArray array];
    for (NSUInteger index=0;index<4;index++) {
        NSEntityDescription *entity=[NSEntityDescription new];
        entity.name=EntityNames()[index]; entity.managedObjectClassName=@"NSManagedObject";
        entity.properties=@[Attribute(@"optionalValue",TypeAt(index),YES),Attribute(@"requiredValue",TypeAt(index),NO)];
        [entities addObject:entity]; [entity release];
    }
    NSManagedObjectModel *model=[NSManagedObjectModel new]; model.entities=entities; return [model autorelease];
}
static BOOL Getters(NSManagedObjectModel *model) {
    if (!model || model.entities.count!=4) return NO;
    for (NSUInteger index=0;index<4;index++) {
        NSEntityDescription *entity=model.entitiesByName[EntityNames()[index]];
        NSAttributeDescription *optional=entity.attributesByName[@"optionalValue"], *required=entity.attributesByName[@"requiredValue"];
        if (entity.properties.count!=2 || !optional || !required || optional.attributeType!=TypeAt(index) || required.attributeType!=TypeAt(index) || !optional.optional || required.optional) return NO;
    }
    return YES;
}
static NSData *Archive(id object) {
    NSError *error=nil;
    NSData *data=[NSKeyedArchiver archivedDataWithRootObject:object requiringSecureCoding:NO error:&error];
    return error ? nil : data;
}
static id Unarchive(NSData *data) {
    if (!data) return nil;
    NSError *error=nil;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    id object=[NSKeyedUnarchiver unarchiveTopLevelObjectWithData:data error:&error];
#pragma clang diagnostic pop
    return error ? nil : object;
}
static NSManagedObjectContext *Context(NSPersistentStoreCoordinator *coordinator) {
    NSManagedObjectContext *context=[[[NSManagedObjectContext alloc] initWithConcurrencyType:NSMainQueueConcurrencyType] autorelease];
    context.persistentStoreCoordinator=coordinator; return context;
}
static NSArray *Values(void) {
    return @[@"authored value",[NSDate dateWithTimeIntervalSinceReferenceDate:-100.25],[NSURL URLWithString:@"https://example.invalid/a%20b?q=1#part"],@YES];
}
static BOOL NilValidation(NSManagedObjectModel *model,NSURL *directory,NSString *route) {
    NSError *error=nil;
    if (![[NSFileManager defaultManager] createDirectoryAtURL:directory withIntermediateDirectories:YES attributes:nil error:&error] || error) return NO;
    NSPersistentStoreCoordinator *coordinator=[[[NSPersistentStoreCoordinator alloc] initWithManagedObjectModel:model] autorelease];
    NSURL *storeURL=[directory URLByAppendingPathComponent:[route stringByAppendingString:@".sqlite"]];
    if (![coordinator addPersistentStoreWithType:NSSQLiteStoreType configuration:nil URL:storeURL options:nil error:&error] || error) return NO;
    NSManagedObjectContext *context=Context(coordinator);
    for (NSUInteger index=0;index<4;index++) {
        NSManagedObject *object=[NSEntityDescription insertNewObjectForEntityForName:EntityNames()[index] inManagedObjectContext:context];
        [object setValue:Values()[index] forKey:@"requiredValue"];
        [object setValue:nil forKey:@"optionalValue"];
        error=nil;
        if (![object validateForInsert:&error] || error) return NO;
    }
    error=nil;
    if (![context save:&error] || error || context.hasChanges) return NO;
    NSManagedObjectContext *fresh=Context(coordinator);
    for (NSUInteger index=0;index<4;index++) {
        error=nil;
        NSArray *objects=[fresh executeFetchRequest:[NSFetchRequest fetchRequestWithEntityName:EntityNames()[index]] error:&error];
        if (error || objects.count!=1 || [objects[0] valueForKey:@"optionalValue"] || ![[objects[0] valueForKey:@"requiredValue"] isEqual:Values()[index]]) return NO;
    }
    for (NSUInteger index=0;index<4;index++) {
        NSArray *objects=[fresh executeFetchRequest:[NSFetchRequest fetchRequestWithEntityName:EntityNames()[index]] error:&error];
        [objects[0] setValue:Values()[index] forKey:@"optionalValue"];
        error=nil;
        if (![objects[0] validateForUpdate:&error] || error) return NO;
    }
    error=nil;
    if (![fresh save:&error] || error || fresh.hasChanges) return NO;
    NSManagedObjectContext *nonNil=Context(coordinator);
    for (NSUInteger index=0;index<4;index++) {
        error=nil;
        NSArray *objects=[nonNil executeFetchRequest:[NSFetchRequest fetchRequestWithEntityName:EntityNames()[index]] error:&error];
        if (error || objects.count!=1 || ![[objects[0] valueForKey:@"optionalValue"] isEqual:Values()[index]] || ![[objects[0] valueForKey:@"requiredValue"] isEqual:Values()[index]]) return NO;
        [objects[0] setValue:nil forKey:@"optionalValue"];
    }
    error=nil;
    if (![nonNil save:&error] || error || nonNil.hasChanges) return NO;
    NSManagedObjectContext *invalid=Context(coordinator);
    NSManagedObject *missing=[NSEntityDescription insertNewObjectForEntityForName:@"StringCase" inManagedObjectContext:invalid];
    [missing setValue:nil forKey:@"requiredValue"]; [missing setValue:nil forKey:@"optionalValue"];
    error=nil;
    if ([missing validateForInsert:&error] || !error) return NO;
    __block NSUInteger saves=0;
    id observer=[[NSNotificationCenter defaultCenter] addObserverForName:NSManagedObjectContextDidSaveNotification object:invalid queue:nil usingBlock:^(NSNotification *note) { (void)note; saves++; }];
    error=nil;
    BOOL failed=![invalid save:&error] && error && invalid.hasChanges && [invalid.insertedObjects containsObject:missing] && saves==0;
    [missing setValue:Values()[0] forKey:@"requiredValue"];
    error=nil;
    BOOL retried=failed && [missing validateForInsert:&error] && !error;
    error=nil;
    retried=retried && [invalid save:&error] && !error && !invalid.hasChanges && saves==1;
    [[NSNotificationCenter defaultCenter] removeObserver:observer];
    if (!retried) return NO;
    NSArray *objects=[Context(coordinator) executeFetchRequest:[NSFetchRequest fetchRequestWithEntityName:@"StringCase"] error:&error];
    if (error || objects.count!=2) return NO;
    for (NSManagedObject *object in objects) if ([object valueForKey:@"optionalValue"] || ![[object valueForKey:@"requiredValue"] isEqual:Values()[0]]) return NO;
    return YES;
}
static BOOL Identifier(NSString *key) {
    if (![key isKindOfClass:NSString.class] || key.length<1 || key.length>64) return NO;
    const char *bytes=key.UTF8String;
    for (NSUInteger index=0;bytes[index];index++) {
        unsigned char value=bytes[index];
        BOOL letter=(value>='A' && value<='Z') || (value>='a' && value<='z') || value=='_';
        if (!letter && !(index && value>='0' && value<='9')) return NO;
    }
    return YES;
}
static NSDictionary *Scalar(NSNumber *number) {
    if (![number isKindOfClass:NSNumber.class]) return nil;
    if (CFGetTypeID((CFTypeRef)number)==CFBooleanGetTypeID()) return @{@"kind":@"boolean",@"value":@([number boolValue])};
    if (CFNumberIsFloatType((CFNumberRef)number) || [number compare:@(INT32_MIN)]==NSOrderedAscending || [number compare:@(INT32_MAX)]==NSOrderedDescending) return nil;
    return @{@"kind":@"integer",@"value":@([number intValue])};
}
static BOOL PathKey(NSString *key) {
    if (![key isKindOfClass:NSString.class] || key.length<1 || key.length>64) return NO;
    const char *bytes=key.UTF8String;
    for (NSUInteger index=0;bytes[index];index++) {
        unsigned char value=bytes[index];
        BOOL letter=(value>='A' && value<='Z') || (value>='a' && value<='z') || value=='_' || value=='$';
        if (!letter && !(index && value>='0' && value<='9')) return NO;
    }
    return YES;
}
static NSString *PathString(NSArray *path) {
    if (path.count>64) return nil;
    NSData *data=[NSJSONSerialization dataWithJSONObject:path options:NSJSONWritingSortedKeys error:NULL];
    if (!data || data.length>512) return nil;
    return [[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease];
}
static void SetReason(NSString **reason,NSString *value) { if (!*reason) *reason=value; }
static void Collect(id node,NSMutableDictionary *fields,NSArray *path,NSUInteger depth,NSUInteger *budget,NSString **reason) {
    if (depth>64) { SetReason(reason,@"depth-limit"); *budget=0; return; }
    if (!*budget) { SetReason(reason,@"node-limit"); return; }
    (*budget)--;
    if ([node isKindOfClass:NSDictionary.class]) {
        if ([node count]>10000) { SetReason(reason,@"collection-limit"); *budget=0; return; }
        for (id key in node) {
            if (!PathKey(key)) { SetReason(reason,@"key-shape"); *budget=0; return; }
            id value=[node objectForKey:key];
            NSArray *next=[path arrayByAddingObject:@{@"key":key}];
            NSString *canonical=PathString(next);
            if (!canonical) { SetReason(reason,@"path-limit"); *budget=0; return; }
            if (Identifier(key)) {
                NSDictionary *scalar=Scalar(value);
                NSMutableArray *values=[NSMutableArray arrayWithObject:scalar ?: (id)[NSNull null]];
                fields[canonical]=@{@"field":key,@"path":next,@"values":values};
            }
            Collect(value,fields,next,depth+1,budget,reason);
        }
    } else if ([node isKindOfClass:NSArray.class]) {
        if ([node count]>10000) { SetReason(reason,@"collection-limit"); *budget=0; return; }
        NSUInteger index=0;
        for (id value in node) {
            NSArray *next=[path arrayByAddingObject:@{@"index":@(index++)}];
            if (!PathString(next)) { SetReason(reason,@"path-limit"); *budget=0; return; }
            Collect(value,fields,next,depth+1,budget,reason);
        }
    }
}
static NSDictionary *Fields(NSData *archive,NSString **reason) {
    if (!archive || archive.length>1024*1024) { SetReason(reason,@"input-limit"); return nil; }
    NSError *error=nil;
    id plist=[NSPropertyListSerialization propertyListWithData:archive options:NSPropertyListImmutable format:NULL error:&error];
    if (!plist || error) { SetReason(reason,@"plist-rejected"); return nil; }
    NSMutableDictionary *fields=[NSMutableDictionary dictionary]; NSUInteger budget=10000;
    Collect(plist,fields,@[],0,&budget,reason);
    if (!budget) { SetReason(reason,@"node-limit"); return nil; }
    return fields;
}
static NSDictionary *State(NSArray *values) {
    if (!values.count) return @{@"present":@NO};
    if (values.count!=1 || ![values[0] isKindOfClass:NSDictionary.class]) return nil;
    return @{@"present":@YES,@"kind":values[0][@"kind"],@"value":values[0][@"value"]};
}
static BOOL AttributeRoundtrip(NSData *archive,NSAttributeType type,BOOL optional) {
    id object=Unarchive(archive);
    return [object isKindOfClass:NSAttributeDescription.class] && [[object name] isEqual:@"probeValue"] && [object attributeType]==type && [object isOptional]==optional;
}
static NSDictionary *CompareBuffers(NSArray *buffers) {
    NSMutableDictionary *result=[NSMutableDictionary dictionaryWithDictionary:@{@"status":@"unparseable",@"reason":@"plist-rejected",@"field":[NSNull null],@"path":[NSNull null],@"required":[NSNull null],@"optional":[NSNull null]}];
    NSString *reasons[4]={nil,nil,nil,nil};
    NSDictionary *a=Fields(buffers[0],&reasons[0]), *b=Fields(buffers[1],&reasons[1]), *a2=Fields(buffers[2],&reasons[2]), *b2=Fields(buffers[3],&reasons[3]);
    NSDictionary *inputs[4]={a,b,a2,b2};
    for (NSUInteger index=0;index<4;index++) if (!inputs[index]) { result[@"reason"]=reasons[index]; return result; }
    result[@"status"]=@"inconclusive"; result[@"reason"]=@"zero-candidates";
    NSMutableSet *keys=[NSMutableSet setWithArray:a.allKeys];
    for (NSDictionary *fields in @[b,a2,b2]) [keys addObjectsFromArray:fields.allKeys];
    NSMutableArray *candidates=[NSMutableArray array];
    for (NSString *key in keys) {
        NSDictionary *one=State(a[key][@"values"]), *two=State(b[key][@"values"]);
        if (!one || !two) {
            id values=a[key][@"values"] ?: @[];
            if (![values isEqual:(b[key][@"values"] ?: @[])] || ![values isEqual:(a2[key][@"values"] ?: @[])] || ![values isEqual:(b2[key][@"values"] ?: @[])]) { result[@"reason"]=@"unsupported-position"; return result; }
            continue;
        }
        if (![one isEqual:State(a2[key][@"values"])] || ![two isEqual:State(b2[key][@"values"])]) { result[@"reason"]=@"repeat-mismatch"; return result; }
        if (![one isEqual:two]) {
            NSDictionary *position=a[key] ?: b[key];
            [candidates addObject:@{@"field":position[@"field"],@"path":position[@"path"],@"required":one,@"optional":two}];
        }
    }
    if (candidates.count==1) { [result addEntriesFromDictionary:candidates[0]]; result[@"status"]=@"observed"; result[@"reason"]=@"unique-scalar"; }
    else if (candidates.count>1) result[@"reason"]=@"multiple-candidates";
    return result;
}
static NSDictionary *Observe(NSUInteger index,BOOL *controls) {
    NSAttributeDescription *required=Attribute(@"probeValue",TypeAt(index),NO), *optional=Attribute(@"probeValue",TypeAt(index),YES);
    if (required.optional || !optional.optional || required.defaultValue || optional.defaultValue) return nil;
    NSData *a=Archive(required), *b=Archive(optional), *a2=Archive(required), *b2=Archive(optional);
    if (!AttributeRoundtrip(a,TypeAt(index),NO) || !AttributeRoundtrip(b,TypeAt(index),YES) || !AttributeRoundtrip(a2,TypeAt(index),NO) || !AttributeRoundtrip(b2,TypeAt(index),YES)) return nil;
    *controls=YES;
    NSMutableDictionary *result=[NSMutableDictionary dictionaryWithDictionary:CompareBuffers(@[a,b,a2,b2])];
    result[@"attributeType"]=TypeNames()[index];
    result[@"requiredArchiveSha256"]=Digest(a); result[@"optionalArchiveSha256"]=Digest(b);
    return result;
}

static BOOL SingleGetters(NSManagedObjectModel *model,BOOL optional) {
    NSEntityDescription *entity=model.entitiesByName[@"SingleCase"];
    NSAttributeDescription *attribute=entity.attributesByName[@"probeValue"];
    return model.entities.count==1 && entity.properties.count==1 && attribute && attribute.attributeType==NSStringAttributeType && attribute.optional==optional && !attribute.defaultValue;
}
static BOOL SingleNilValidation(NSManagedObjectModel *model,BOOL optional,NSURL *work,NSString *name) {
    NSError *error=nil;
    NSPersistentStoreCoordinator *coordinator=[[[NSPersistentStoreCoordinator alloc] initWithManagedObjectModel:model] autorelease];
    if (![coordinator addPersistentStoreWithType:NSSQLiteStoreType configuration:nil URL:[work URLByAppendingPathComponent:[name stringByAppendingString:@".sqlite"]] options:nil error:&error] || error) return NO;
    NSManagedObjectContext *context=Context(coordinator);
    NSManagedObject *object=[NSEntityDescription insertNewObjectForEntityForName:@"SingleCase" inManagedObjectContext:context];
    [object setValue:nil forKey:@"probeValue"];
    error=nil; BOOL valid=[object validateForInsert:&error];
    if (optional ? (!valid || error) : (valid || !error)) return NO;
    error=nil; BOOL saved=[context save:&error];
    if (optional) return saved && !error && !context.hasChanges;
    if (saved || !error || !context.hasChanges || ![context.insertedObjects containsObject:object]) return NO;
    [object setValue:@"known required" forKey:@"probeValue"];
    error=nil; return [context save:&error] && !error && !context.hasChanges;
}
static int CompiledControls(int argc,const char **argv) {
    if (argc!=12) return 2;
    NSURL *output=[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[2]]], *work=[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[3]]];
    NSMutableDictionary *report=[NSMutableDictionary dictionaryWithDictionary:@{@"format":@1,@"result":@"failed",@"failure":@"api-failure",@"stage":@"inputs",@"scope":@"fresh-single-string-attribute-compiled-buffers-only",@"controls":@NO,@"platforms":[NSMutableDictionary dictionary]}];
    @try {
        NSError *error=nil;
        if (![[NSFileManager defaultManager] createDirectoryAtURL:work withIntermediateDirectories:YES attributes:nil error:&error] || error) @throw @"control";
        NSMutableArray *all=[NSMutableArray array];
        for (NSUInteger index=0;index<8;index++) {
            NSData *data=[NSData dataWithContentsOfFile:[NSString stringWithUTF8String:argv[index+4]]];
            if (!Digest(data)) @throw @"control";
            [all addObject:data];
        }
        report[@"stage"]=@"public-controls";
        for (NSUInteger index=0;index<8;index++) {
            NSURL *url=[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[index+4]]];
            NSManagedObjectModel *model=[[[NSManagedObjectModel alloc] initWithContentsOfURL:url] autorelease];
            BOOL optional=index%2==1;
            if (!SingleGetters(model,optional) || !SingleGetters(Unarchive(Archive(model)),optional) || !SingleNilValidation(model,optional,work,[NSString stringWithFormat:@"case%lu",(unsigned long)index])) @throw @"control";
        }
        report[@"controls"]=@YES;
        report[@"stage"]=@"bounded-observation";
        for (NSUInteger platform=0;platform<2;platform++) {
            NSArray *buffers=[all subarrayWithRange:NSMakeRange(platform*4,4)];
            NSMutableDictionary *fact=[NSMutableDictionary dictionaryWithDictionary:CompareBuffers(buffers)];
            NSMutableArray *hashes=[NSMutableArray array]; for (NSData *buffer in buffers) [hashes addObject:Digest(buffer)];
            fact[@"outputHashes"]=hashes;
            report[@"platforms"][platform ? @"ios" : @"mac"]=fact;
        }
        report[@"result"]=@"pass"; report[@"failure"]=@"none"; report[@"stage"]=@"complete";
    } @catch (id exception) { (void)exception; report[@"failure"]=@"control-failure"; }
    NSError *error=nil; NSData *data=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingSortedKeys error:&error];
    if (!data || error || data.length>16384 || ![data writeToURL:output atomically:YES]) return 3;
    return [report[@"result"] isEqual:@"pass"] ? 0 : 1;
}
static BOOL Run(NSMutableDictionary *report,NSURL *macURL,NSURL *iosURL,NSURL *workURL) {
    NSMutableDictionary *checks=report[@"checks"];
    Stage=@"programmatic.getters";
    NSManagedObjectModel *model=Model();
    if (!Getters(model)) return NO; checks[@"programmaticGetters"]=@YES;
    Stage=@"programmatic.archive";
    id roundtrip=Unarchive(Archive(model));
    if (![roundtrip isKindOfClass:NSManagedObjectModel.class] || !Getters(roundtrip)) return NO; checks[@"programmaticRoundtrip"]=@YES;
    Stage=@"archive.controls";
    for (NSUInteger index=0;index<4;index++) {
        BOOL controls=NO; NSDictionary *observation=Observe(index,&controls);
        if (!controls || !observation) return NO; [report[@"archiveFacts"] addObject:observation];
    }
    checks[@"archiveObservationControls"]=@YES;
    Stage=@"programmatic.validation";
    if (!NilValidation(roundtrip,workURL,@"programmatic")) return NO; checks[@"programmaticNilValidation"]=@YES;
    Stage=@"mac.load";
    NSManagedObjectModel *mac=[[[NSManagedObjectModel alloc] initWithContentsOfURL:macURL] autorelease];
    if (!mac) return NO;
    Stage=@"mac.getters";
    if (!Getters(mac)) return NO; checks[@"macCompiledGetters"]=@YES;
    Stage=@"mac.validation";
    if (!NilValidation(mac,workURL,@"mac")) return NO; checks[@"macCompiledNilValidation"]=@YES;
    Stage=@"ios.load";
    NSManagedObjectModel *ios=[[[NSManagedObjectModel alloc] initWithContentsOfURL:iosURL] autorelease];
    if (!ios) { report[@"result"]=@"incomplete"; report[@"failure"]=@"ios-resource-rejected"; return NO; }
    Stage=@"ios.getters";
    if (!Getters(ios)) return NO; checks[@"iosCompiledGetters"]=@YES;
    Stage=@"ios.validation";
    if (!NilValidation(ios,workURL,@"ios")) return NO; checks[@"iosCompiledNilValidation"]=@YES;
    Stage=@"complete"; report[@"result"]=@"pass"; report[@"failure"]=@"none"; return YES;
}
int main(int argc,const char **argv) {
    @autoreleasepool {
        if (argc>1 && strcmp(argv[1],"--compiled-controls")==0) return CompiledControls(argc,argv);
        if (argc!=5) return 2;
        NSArray *checkNames=@[@"programmaticGetters",@"programmaticRoundtrip",@"archiveObservationControls",@"programmaticNilValidation",@"macCompiledGetters",@"macCompiledNilValidation",@"iosCompiledGetters",@"iosCompiledNilValidation"];
        NSMutableDictionary *checks=[NSMutableDictionary dictionary]; for (NSString *name in checkNames) checks[name]=@NO;
        NSMutableDictionary *report=[NSMutableDictionary dictionaryWithDictionary:@{@"format":@1,@"result":@"failed",@"failure":@"api-failure",@"stage":Stage,@"scope":@{@"requiredNilNegative":@"String",@"optionalPersistence":TypeNames(),@"compiledResources":@"public-load-and-hash-only",@"archiveFields":@"detached-authored-attribute-buffers-only"},@"checks":checks,@"archiveFacts":[NSMutableArray array],@"modelHashes":@{@"mac":[NSNull null],@"ios":[NSNull null]}}];
        NSURL *mac=[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]], *ios=[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[2]]], *output=[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[3]]], *work=[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[4]]];
        BOOL success=NO;
        @try {
            Stage=@"resource.inputs";
            NSString *macHash=Digest([NSData dataWithContentsOfURL:mac]), *iosHash=Digest([NSData dataWithContentsOfURL:ios]);
            report[@"modelHashes"]=@{@"mac":macHash ?: [NSNull null],@"ios":iosHash ?: [NSNull null]};
            if (!macHash || !iosHash) report[@"failure"]=@"resource-missing";
            else success=Run(report,mac,ios,work);
        } @catch (id exception) { report[@"result"]=@"failed"; report[@"failure"]=@"exception"; }
        report[@"stage"]=Stage;
        if (![checks[@"archiveObservationControls"] boolValue]) [report[@"archiveFacts"] removeAllObjects];
        NSError *error=nil; NSData *data=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingSortedKeys error:&error];
        if (!data || error || data.length>32768 || ![data writeToURL:output atomically:YES]) return 3;
        return success ? 0 : 1;
    }
}
