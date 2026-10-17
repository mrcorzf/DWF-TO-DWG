// SPDX-License-Identifier: MIT
// Native Windows interface; conversion runs in an isolated child process.
#define UNICODE
#define _UNICODE
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <algorithm>
#include <string>
#include <vector>

namespace {
enum { INPUT=101, OUTPUT, FORMAT, BROWSE, SAVEAS, CONVERT, CANCEL, DETAILS, OPENFOLDER, REVEAL };
enum { STAGE=WM_APP+1, DONE };
HWND window,input,output,format,browse,saveas,convert,cancel,details,foldercheck,reveal,progress,status,logbox;
HWND title,subtitle,inlabel,outlabel,formatlabel,hint;
HFONT font,titlefont;HBRUSH background;
HANDLE job=0,thread=0;bool busy=false,expanded=false,closing=false;
volatile LONG cancelled=0;int dpi=96;
std::wstring selected, destination;
int px(int value){return MulDiv(value,dpi,96);}
bool exists(const std::wstring& p){return GetFileAttributesW(p.c_str())!=INVALID_FILE_ATTRIBUTES;}
std::wstring text(HWND control){int n=GetWindowTextLengthW(control);std::vector<wchar_t>b(n+1);GetWindowTextW(control,b.data(),n+1);return b.data();}
std::wstring fromutf8(const std::string& s){int n=MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),0,0);std::wstring out(n,L' ');if(n)MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),&out[0],n);return out;}
std::wstring extension(){return SendMessageW(format,CB_GETCURSEL,0,0)==1?L".dxf":L".dwg";}
std::wstring parent(const std::wstring& p){auto pos=p.find_last_of(L"\\/");return pos==std::wstring::npos?L".":p.substr(0,pos);}
std::wstring without_extension(std::wstring p){auto slash=p.find_last_of(L"\\/"),dot=p.find_last_of(L'.');if(dot!=std::wstring::npos&&(slash==std::wstring::npos||dot>slash))p.resize(dot);return p;}
std::wstring suggested(){std::wstring stem=without_extension(selected)+L"_convertido",p=stem+extension();for(int i=2;exists(p);++i)p=stem+L"_"+std::to_wstring(i)+extension();return p;}
void setsource(const std::wstring& path){selected=path;SetWindowTextW(input,path.c_str());destination=suggested();SetWindowTextW(output,destination.c_str());SetWindowTextW(status,L"Pronto. Clique em Converter para criar seu desenho 3D.");EnableWindow(convert,TRUE);EnableWindow(reveal,FALSE);}
void openfolder(){if(!destination.empty())ShellExecuteW(window,L"open",parent(destination).c_str(),0,0,SW_SHOWNORMAL);}
HWND control(const wchar_t* cls,const wchar_t* label,DWORD style,int id=0){HWND h=CreateWindowExW(wcscmp(cls,L"EDIT")==0?WS_EX_CLIENTEDGE:0,cls,label,WS_CHILD|WS_VISIBLE|style,0,0,0,0,window,(HMENU)(INT_PTR)id,GetModuleHandleW(0),0);SendMessageW(h,WM_SETFONT,(WPARAM)font,TRUE);return h;}
void place(HWND h,int x,int y,int w,int height){MoveWindow(h,px(x),px(y),px(w),px(height),TRUE);}
void layout(){RECT r;GetClientRect(window,&r);int w=MulDiv(r.right,96,dpi),margin=28,bwidth=148;
    place(title,margin,23,w-2*margin,38);place(subtitle,margin,65,w-2*margin,25);
    place(inlabel,margin,112,w-2*margin,22);place(input,margin,140,w-2*margin-bwidth-12,32);place(browse,w-margin-bwidth,140,bwidth,32);
    place(formatlabel,margin,195,180,22);place(format,margin,222,w-2*margin,170);
    place(outlabel,margin,270,w-2*margin,22);place(output,margin,298,w-2*margin-bwidth-12,32);place(saveas,w-margin-bwidth,298,bwidth,32);
    place(foldercheck,margin,345,w-2*margin,24);place(hint,margin,379,w-2*margin,42);
    place(status,margin,430,w-2*margin,42);place(progress,margin,480,w-2*margin,14);
    place(details,margin,515,120,34);place(reveal,margin+132,515,145,34);place(cancel,w-margin-270,515,110,34);place(convert,w-margin-148,515,148,34);
    place(logbox,margin,572,w-2*margin,160);ShowWindow(logbox,expanded?SW_SHOW:SW_HIDE);
}
void setbusy(bool value){busy=value;for(HWND h:{browse,saveas,format,convert})EnableWindow(h,!value);EnableWindow(convert,!value&&!selected.empty());EnableWindow(cancel,value);SendMessageW(progress,PBM_SETMARQUEE,value,40);if(!value)SendMessageW(progress,PBM_SETPOS,0,0);}
void choose_input(){wchar_t path[32768]={0};OPENFILENAMEW ofn={};ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=window;ofn.lpstrFile=path;ofn.nMaxFile=32768;ofn.lpstrFilter=L"Desenhos DWF (*.dwf)\0*.dwf\0\0";ofn.lpstrTitle=L"Selecione o desenho 3D em DWF";ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;if(GetOpenFileNameW(&ofn))setsource(path);}
void choose_output(){if(selected.empty()){choose_input();if(selected.empty())return;}wchar_t path[32768]={0};wcsncpy(path,destination.c_str(),32767);std::wstring ext=extension();OPENFILENAMEW ofn={};ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=window;ofn.lpstrFile=path;ofn.nMaxFile=32768;ofn.lpstrFilter=ext==L".dwg"?L"Desenho AutoCAD (*.dwg)\0*.dwg\0\0":L"Desenho DXF (*.dxf)\0*.dxf\0\0";ofn.lpstrDefExt=ext.c_str()+1;ofn.lpstrTitle=L"Onde salvar o desenho convertido?";ofn.Flags=OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;if(GetSaveFileNameW(&ofn)){destination=path;SetWindowTextW(output,path);}}
struct Work{std::wstring source,target,engine;HANDLE job;};
struct Result{bool ok,cancelled;std::wstring message,log;};
void poststatus(const std::wstring& value){auto p=new std::wstring(value);if(!PostMessageW(window,STAGE,0,(LPARAM)p))delete p;}
DWORD WINAPI worker(void* parameter){Work work=*(Work*)parameter;delete (Work*)parameter;
    Result* result=new Result{false,false,L"Não foi possível concluir a conversão.",L""};
    std::wstring temp=without_extension(work.target)+L".dwf3d-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+work.target.substr(work.target.find_last_of(L'.'));
    HANDLE read=0,write=0,nul=0;PROCESS_INFORMATION pi={};std::string captured,pending;
    SECURITY_ATTRIBUTES security={sizeof(security),0,TRUE};
    if(CreatePipe(&read,&write,&security,0)&&SetHandleInformation(read,HANDLE_FLAG_INHERIT,0)){
        nul=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,0);
        STARTUPINFOW si={};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;si.hStdInput=nul;si.hStdOutput=write;si.hStdError=write;
        std::wstring command=L"\""+work.engine+L"\" \""+work.source+L"\" \""+temp+L"\"";std::vector<wchar_t> cmd(command.begin(),command.end());cmd.push_back(0);
        if(CreateProcessW(work.engine.c_str(),cmd.data(),0,0,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED,0,0,&si,&pi)){
            if(AssignProcessToJobObject(work.job,pi.hProcess)){
                ResumeThread(pi.hThread);CloseHandle(write);write=0;
                if(InterlockedCompareExchange(&cancelled,0,0))TerminateJobObject(work.job,ERROR_CANCELLED);
                char buffer[4096];DWORD count;
                while(ReadFile(read,buffer,sizeof(buffer),&count,0)&&count){
                    if(captured.size()<1000000)captured.append(buffer,std::min<size_t>(count,1000000-captured.size()));
                    pending.append(buffer,count);size_t newline;
                    while((newline=pending.find('\n'))!=std::string::npos){std::string line=pending.substr(0,newline);pending.erase(0,newline+1);if(line.compare(0,6,"ETAPA:")==0)poststatus(fromutf8(line.substr(6)));}
                    if(pending.size()>65536)pending.clear();
                }
                WaitForSingleObject(pi.hProcess,INFINITE);DWORD exitcode=1;GetExitCodeProcess(pi.hProcess,&exitcode);
                result->cancelled=InterlockedCompareExchange(&cancelled,0,0)!=0;
                if(exitcode==0&&!result->cancelled&&exists(temp)){
                    poststatus(L"Salvando o desenho na pasta escolhida...");
                    if(MoveFileW(temp.c_str(),work.target.c_str())){result->ok=true;result->message=L"Conversão concluída. Seu desenho 3D está pronto para abrir no CAD.";}
                    else result->message=L"O desenho foi convertido, mas não foi possível salvar no destino. Confira a pasta e escolha um nome que ainda não exista.";
                }else if(result->cancelled)result->message=L"Conversão cancelada. Nenhum arquivo de destino foi substituído.";
                else result->message=L"Não foi possível converter este desenho. Consulte os detalhes abaixo para identificar o motivo.";
            }else{TerminateProcess(pi.hProcess,1);result->message=L"O Windows não permitiu iniciar a conversão com segurança. Feche o programa e tente novamente.";}
        }else result->message=L"Não foi possível iniciar o conversor. Extraia todos os arquivos do ZIP para a mesma pasta.";
    }else result->message=L"Não foi possível preparar a conversão. Feche o programa e tente novamente.";
    if(pi.hThread)CloseHandle(pi.hThread);if(pi.hProcess)CloseHandle(pi.hProcess);if(read)CloseHandle(read);if(write)CloseHandle(write);if(nul&&nul!=INVALID_HANDLE_VALUE)CloseHandle(nul);
    DeleteFileW(temp.c_str());result->log=fromutf8(captured);PostMessageW(window,DONE,0,(LPARAM)result);return 0;
}
void start(){if(selected.empty()){choose_input();if(selected.empty())return;}if(destination.empty()){choose_output();if(destination.empty())return;}
    if(exists(destination)){MessageBoxW(window,L"Já existe um arquivo com esse nome. Use “Salvar como…” para escolher outro nome.",L"Escolha outro nome",MB_OK|MB_ICONINFORMATION);return;}
    wchar_t module[32768]={0};GetModuleFileNameW(0,module,32768);std::wstring engine=parent(module)+L"\\dwfcore.exe";
    if(!exists(engine)){MessageBoxW(window,L"dwfcore.exe não foi encontrado. Extraia o ZIP completo e mantenha os arquivos na mesma pasta.",L"Pacote incompleto",MB_OK|MB_ICONERROR);return;}
    job=CreateJobObjectW(0,0);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!job||!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))){if(job)CloseHandle(job);job=0;MessageBoxW(window,L"Não foi possível preparar a conversão.",L"Erro",MB_OK|MB_ICONERROR);return;}
    InterlockedExchange(&cancelled,0);SetWindowTextW(logbox,L"");SetWindowTextW(status,L"Iniciando conversão...");EnableWindow(reveal,FALSE);setbusy(true);
    Work* work=new Work{selected,destination,engine,job};thread=CreateThread(0,0,worker,work,0,0);
    if(!thread){delete work;CloseHandle(job);job=0;setbusy(false);SetWindowTextW(status,L"Não foi possível iniciar. Tente novamente.");}
}
void abort_work(){if(busy){InterlockedExchange(&cancelled,1);SetWindowTextW(status,L"Cancelando conversão...");if(job)TerminateJobObject(job,ERROR_CANCELLED);EnableWindow(cancel,FALSE);}}
LRESULT CALLBACK proc(HWND h,UINT msg,WPARAM wp,LPARAM lp){switch(msg){
    case WM_CREATE:{window=h;HDC dc=GetDC(h);dpi=GetDeviceCaps(dc,LOGPIXELSX);ReleaseDC(h,dc);font=CreateFontW(-px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");titlefont=CreateFontW(-px(27),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");background=CreateSolidBrush(RGB(248,250,253));
        title=control(L"STATIC",L"DWF 3D → DWG / DXF",0);SendMessageW(title,WM_SETFONT,(WPARAM)titlefont,TRUE);
        subtitle=control(L"STATIC",L"Escolha seu desenho, defina onde salvar e clique em Converter.",0);
        inlabel=control(L"STATIC",L"1. Arquivo DWF   ·   Você também pode arrastar o arquivo para esta janela",0);
        input=control(L"EDIT",L"Nenhum arquivo selecionado",ES_AUTOHSCROLL|ES_READONLY,INPUT);browse=control(L"BUTTON",L"Selecionar DWF…",WS_TABSTOP,BROWSE);
        formatlabel=control(L"STATIC",L"2. Formato de saída",0);format=control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,FORMAT);SendMessageW(format,CB_ADDSTRING,0,(LPARAM)L"DWG — desenho para AutoCAD");SendMessageW(format,CB_ADDSTRING,0,(LPARAM)L"DXF — desenho 3D para intercâmbio");SendMessageW(format,CB_SETCURSEL,0,0);
        outlabel=control(L"STATIC",L"3. Onde salvar o desenho convertido",0);output=control(L"EDIT",L"A pasta do DWF será sugerida automaticamente",ES_AUTOHSCROLL|ES_READONLY,OUTPUT);saveas=control(L"BUTTON",L"Salvar como…",WS_TABSTOP,SAVEAS);
        foldercheck=control(L"BUTTON",L"Abrir a pasta de destino ao concluir",BS_AUTOCHECKBOX|WS_TABSTOP,OPENFOLDER);SendMessageW(foldercheck,BM_SETCHECK,BST_CHECKED,0);
        hint=control(L"STATIC",L"Preserva a geometria 3D em malhas e círculos. Não recupera sólidos paramétricos, cores ou dados BIM.",0);
        status=control(L"STATIC",L"Selecione um arquivo DWF para começar.",0);progress=control(PROGRESS_CLASSW,L"",PBS_MARQUEE);
        details=control(L"BUTTON",L"Ver detalhes",WS_TABSTOP,DETAILS);reveal=control(L"BUTTON",L"Abrir pasta",WS_TABSTOP,REVEAL);cancel=control(L"BUTTON",L"Cancelar",WS_TABSTOP,CANCEL);convert=control(L"BUTTON",L"Converter",BS_DEFPUSHBUTTON|WS_TABSTOP,CONVERT);
        logbox=control(L"EDIT",L"",ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL,0);SendMessageW(logbox,EM_SETLIMITTEXT,1000000,0);ShowWindow(logbox,SW_HIDE);EnableWindow(cancel,FALSE);EnableWindow(convert,FALSE);EnableWindow(reveal,FALSE);DragAcceptFiles(h,TRUE);layout();return 0;}
    case WM_SIZE:if(input)layout();return 0;
    case WM_GETMINMAXINFO:((MINMAXINFO*)lp)->ptMinTrackSize={px(800),px(expanded?800:620)};return 0;
    case WM_CTLCOLORSTATIC:SetTextColor((HDC)wp,RGB(35,49,67));SetBkColor((HDC)wp,RGB(248,250,253));return (LRESULT)background;
    case WM_ERASEBKGND:{RECT r;GetClientRect(h,&r);FillRect((HDC)wp,&r,background?background:(HBRUSH)(COLOR_WINDOW+1));return 1;}
    case WM_DROPFILES:{if(!busy){wchar_t path[32768]={0};if(DragQueryFileW((HDROP)wp,0,path,32768))setsource(path);}DragFinish((HDROP)wp);return 0;}
    case WM_COMMAND:switch(LOWORD(wp)){
        case BROWSE:if(!busy)choose_input();break;
        case SAVEAS:if(!busy)choose_output();break;
        case FORMAT:if(HIWORD(wp)==CBN_SELCHANGE&&!selected.empty()){destination=without_extension(destination)+extension();if(exists(destination))destination=suggested();SetWindowTextW(output,destination.c_str());}break;
        case IDOK:case CONVERT:if(!busy)start();break;
        case IDCANCEL:if(busy)abort_work();else SendMessageW(h,WM_CLOSE,0,0);break;
        case CANCEL:abort_work();break;
        case REVEAL:openfolder();break;
        case DETAILS:{expanded=!expanded;SetWindowTextW(details,expanded?L"Ocultar detalhes":L"Ver detalhes");RECT r;GetWindowRect(h,&r);SetWindowPos(h,0,0,0,r.right-r.left,px(expanded?800:620),SWP_NOMOVE|SWP_NOZORDER);layout();break;}
    }return 0;
    case STAGE:{auto p=(std::wstring*)lp;if(busy&&!cancelled)SetWindowTextW(status,p->c_str());delete p;return 0;}
    case DONE:{auto result=(Result*)lp;setbusy(false);if(thread){CloseHandle(thread);thread=0;}if(job){CloseHandle(job);job=0;}SetWindowTextW(status,result->message.c_str());SetWindowTextW(logbox,result->log.c_str());EnableWindow(reveal,result->ok);
        if(!result->ok&&!result->cancelled&&!expanded){SendMessageW(h,WM_COMMAND,DETAILS,0);}
        if(result->ok&&SendMessageW(foldercheck,BM_GETCHECK,0,0)==BST_CHECKED&&!closing)openfolder();delete result;if(closing)DestroyWindow(h);return 0;}
    case WM_CLOSE:if(busy){closing=true;abort_work();}else DestroyWindow(h);return 0;
    case WM_DESTROY:if(font)DeleteObject(font);if(titlefont)DeleteObject(titlefont);if(background)DeleteObject(background);PostQuitMessage(0);return 0;
}return DefWindowProcW(h,msg,wp,lp);}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show){SetProcessDPIAware();INITCOMMONCONTROLSEX controls={sizeof(controls),ICC_PROGRESS_CLASS|ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
    WNDCLASSW cls={};cls.lpfnWndProc=proc;cls.hInstance=instance;cls.hCursor=LoadCursorW(0,IDC_ARROW);cls.hIcon=LoadIconW(0,IDI_APPLICATION);cls.lpszClassName=L"Dwf3dConverterUI";RegisterClassW(&cls);
    HDC dc=GetDC(0);dpi=GetDeviceCaps(dc,LOGPIXELSX);ReleaseDC(0,dc);HWND h=CreateWindowExW(0,cls.lpszClassName,L"Conversor DWF 3D — versão 2",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,px(900),px(620),0,0,instance,0);if(!h)return 1;
    int argc=0;LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);if(argv){if(argc==2)setsource(argv[1]);LocalFree(argv);}ShowWindow(h,show);UpdateWindow(h);MSG msg;while(GetMessageW(&msg,0,0,0)>0){if(!IsDialogMessageW(h,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}return 0;
}
