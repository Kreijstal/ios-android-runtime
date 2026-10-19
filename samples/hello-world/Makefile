ARCHS = arm64
TARGET = iphone:clang:latest:14.0
PACKAGE_FORMAT = ipa

include $(THEOS)/makefiles/common.mk

APPLICATION_NAME = HelloWorld
HelloWorld_FILES = main.m
HelloWorld_FRAMEWORKS = UIKit Foundation
HelloWorld_CFLAGS = -fobjc-arc

include $(THEOS_MAKE_PATH)/application.mk
