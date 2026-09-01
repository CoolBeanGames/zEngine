#include "EditorShell.h"
RECT EditorShell::ChromeRectangle(int index) const {
    if(index==11)return {optionsBar_.right-128,optionsBar_.top+4,optionsBar_.right-10,optionsBar_.bottom-4};
    if(index==9)return {};
    if(index==10)return {mediaLibrary_.left+12,mediaLibrary_.top+73,mediaLibrary_.left+42,mediaLibrary_.top+99};
    if(index==0)return CreateObjectRectangle();
    if(index==1 || index==2)return {};
    if(index>=6)return ToolRectangle(index-6);
    const int center=(viewportPanel_.left+viewportPanel_.right)/2;
    const int left=center-42+(index-3)*30;
    return {left,viewportPanel_.top+4,left+28,viewportPanel_.top+26};
}
int EditorShell::ChromeHit(POINT point) const {
    for(int i=0;i<12;++i){const auto r=ChromeRectangle(i);if(PtInRect(&r,point))return i;}
    return -1;
}
bool EditorShell::ChromeEnabled(int index) const {
    if(index==11)return true;
    if(index==9)return false;
    if(index==10)return project_.has_value() && AssetFolder()!=assetsDirectory_;
    if(index==0)return sceneOpen_ && !Playing();
    if(index==1 || index==2)return false;
    if(index==3)return sceneOpen_ && editingPrefab_.empty();
    if(index==4 || index==5)return Playing();
    return !Playing();
}
