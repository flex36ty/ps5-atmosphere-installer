#include "../backend/game_region.h"
#include <assert.h>
#include <stdio.h>
int main(void){
    assert(!strcmp(game_region("UP0000-PPSA12345_00-ABCDEFGHIJKLMNOP"),"US"));
    assert(!strcmp(game_region("EP0000-PPSA12345_00-ABCDEFGHIJKLMNOP"),"EUR"));
    assert(!strcmp(game_region("JP0000-PPSA12345_00-ABCDEFGHIJKLMNOP"),"JPN"));
    assert(!strcmp(game_region("HP0000-PPSA12345_00-ABCDEFGHIJKLMNOP"),"ASIA"));
    assert(!strcmp(game_region("KP0000-CUSA12345_00-ABCDEFGHIJKLMNOP"),"KOR"));
    assert(!*game_region(NULL));assert(!*game_region(""));assert(!*game_region("PPSA12345"));
    assert(!*game_region("UPbad"));assert(!*game_region("XX0000-PPSA12345_00-ABCDEFGHIJKLMNOP"));
    assert(!*game_region("UP0000-PPSA12345_00-ABCDEFGHIJKLMNOPextra"));
    assert(!*game_region("UP0000-PPSA1234X_00-ABCDEFGHIJKLMNOP"));
    puts("PASS: content-ID territories and unknown/malformed region metadata");
}
