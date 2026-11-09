#include "a9_md5.h"
#include <stdio.h>
#include <string.h>
int main(void) {
    static const struct {const char *text; const char *expected;} test[] = {
        {"", "d41d8cd98f00b204e9800998ecf8427e"},
        {"abc", "900150983cd24fb0d6963f7d28e17f72"},
        {"message digest", "f96b697d7cb7938d525a2f31aaf161d0"}
    };
    for (size_t j=0;j<sizeof test/sizeof test[0];j++) {
        uint8_t md5[16];a9_md5(test[j].text,strlen(test[j].text),md5);
        char hex[33];for(unsigned k=0;k<16;k++)sprintf(hex+2*k,"%02x",md5[k]);
        if(strcmp(hex,test[j].expected))return 1;
    }
    puts("PASS: RFC 1321 MD5 known vectors");return 0;
}
