#include "EditorShell.h"
RECT EditorShell::ChromeRectangle(int index) const {
    if(index==0)return CreateObjectRectangle();
    if(index==1)return CreateScriptRectangle();
    if(index==2)return CreateSceneRectangle();
    if(index>=6)return ToolRectangle(index-6);
    const int center=(viewportPanel_.left+viewportPanel_.right)/2;
    const int left=center-42+(index-3)*30;
    return {left,viewportPanel_.top+4,left+28,viewportPanel_.top+26};
}
int EditorShell::ChromeHit(POINT point) const {
    for(int i=0;i<9;++i){const auto r=ChromeRectangle(i);if(PtInRect(&r,point))return i;}
    return -1;
}
bool EditorShell::ChromeEnabled(int index) const {
    if(index==0)return sceneOpen_ && !Playing();
    if(index==1 || index==2)return project_.has_value() && !Playing();
    if(index==3)return sceneOpen_ && editingPrefab_.empty();
    if(index==4 || index==5)return Playing();
    return !Playing();
}
