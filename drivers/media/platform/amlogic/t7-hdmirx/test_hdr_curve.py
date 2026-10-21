#!/usr/bin/env python3
"""Compile the real curve parser against malformed and boundary transactions."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('t7-vdin-test.c').read_text()
parser = source[source.index('static int t7_vdin_parse_hdr_curve'):
                source.index('static ssize_t hdr_curve_store')]
header = Path(__file__).with_name('t7-vdin-hdr.h').read_text()
tables = header[:header.index('static void t7_vdin_hdr_reload')]
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
typedef uint32_t u32;
static char *strim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *end=s+strlen(s);
    while (end>s && isspace((unsigned char)end[-1])) *--end=0;
    return s;
}
static int kstrtou32(const char *s, int base, u32 *v) {
    char *end;errno=0;
    unsigned long x=strtoul(s,&end,base);
    if (!*s || *s=='-' || *end || errno || x>UINT32_MAX) return -EINVAL;
    *v=x;return 0;
}
'''
checks = r'''
int main(void) {
    u32 parsed[298];
    char valid[4096],text[4096];
    unsigned int len=0;
    for (int i=0;i<298;i++) len+=snprintf(valid+len,sizeof(valid)-len,
        "%u%c",i<149?t7_hdr_gain[i]:t7_hdr_srgb[i-149],i==148?'\n':i==297?'\n':',');
    strcpy(text,valid);assert(!t7_vdin_parse_hdr_curve(text,parsed));
    assert(!memcmp(parsed,t7_hdr_gain,sizeof(t7_hdr_gain)));
    assert(!memcmp(parsed+149,t7_hdr_srgb,sizeof(t7_hdr_srgb)));
    assert(t7_hdr_pq[142]==524287);
    for (unsigned int n=0;n<len-1;n++) {
        memcpy(text,valid,n);text[n]=0;
        assert(t7_vdin_parse_hdr_curve(text,parsed)==-EINVAL);
    }
    strcpy(text,valid);strcat(text,"1");assert(t7_vdin_parse_hdr_curve(text,parsed)==-EINVAL);
    strcpy(text,valid);text[0]='x';assert(t7_vdin_parse_hdr_curve(text,parsed)==-EINVAL);
    strcpy(text,valid);text[0]='9';assert(t7_vdin_parse_hdr_curve(text,parsed)==-EINVAL);
    strcpy(text,valid);text[0]='-';assert(t7_vdin_parse_hdr_curve(text,parsed)==-EINVAL);
    strcpy(text,valid);text[len-2]='4';assert(t7_vdin_parse_hdr_curve(text,parsed)==-EINVAL);
    puts("HDR curve transactions: valid pair, truncation, excess, range and endpoint guards passed");
}
'''
with tempfile.TemporaryDirectory(prefix='t7-hdr-curve-') as directory:
    path = Path(directory)
    (path/'test.c').write_text(prefix + tables + parser + checks)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    str(path/'test.c'), '-o', str(path/'test')], check=True)
    subprocess.run([str(path/'test')], check=True)
