#!/usr/bin/python

import re
import os
import sys

if len(sys.argv) != 4:
	print "This script must be called with three arguments!"
	print "Usage:\n   changesize.py file newNs newNt"
	sys.exit(1)
	
inputfile = open(sys.argv[1],'r')

filedata = inputfile.read()

patternx = re.search('glob_x\ *=\ *([0-9]+)',filedata)
patterny = re.search('glob_y\ *=\ *([0-9]+)',filedata)
patternz = re.search('glob_z\ *=\ *([0-9]+)',filedata)
patternt = re.search('glob_t\ *=\ *([0-9]+)',filedata)

if patternx != None:
	replacerule = re.search('(glob_x\ *=\ *[0-9]+)',filedata)
	filedata = filedata.replace(replacerule.group(1),'glob_x = '+sys.argv[2])

if patterny != None:
	replacerule = re.search('(glob_y\ *=\ *[0-9]+)',filedata)
	filedata = filedata.replace(replacerule.group(1),'glob_y = '+sys.argv[2])

if patternz != None:
	replacerule = re.search('(glob_z\ *=\ *[0-9]+)',filedata)
	filedata = filedata.replace(replacerule.group(1),'glob_z = '+sys.argv[2])

if patternt != None:
	replacerule = re.search('(glob_t\ *=\ *[0-9]+)',filedata)
	filedata = filedata.replace(replacerule.group(1),'glob_t = '+sys.argv[3])
	
if (patternx != None) and (patternt != None):
	filedata = filedata.replace(patternx.group(1)+"c"+patternt.group(1),sys.argv[2]+'c'+sys.argv[3])

inputfile.close()

outputfile = open(sys.argv[1],'w')
outputfile.write(filedata)
outputfile.close()




