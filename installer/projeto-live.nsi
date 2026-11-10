; Projeto Live: instalador para Windows (etapa 8).
;
; Coloca de uma vez:
;   - o OBS do Projeto Live (pasta "OBS")
;   - o app de projeção (base FreeShow, pasta "Projecao")
; e também:
;   - libera os dois no firewall do Windows (redes privadas), para a pergunta "permitir rede" não
;     aparecer no meio do culto;
;   - cria os atalhos (menu Iniciar e área de trabalho);
;   - confere o componente da Apple para o iPhone pelo cabo USB e avisa se faltar.
;
; Montado pelo workflow .github/workflows/projeto-live-instalador.yaml, que deixa os arquivos em
; installer\stage\obs e installer\stage\projecao antes de chamar o makensis.

Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

!ifndef VERSION
!define VERSION "0.0.0"
!endif

!define PRODUCT "Projeto Live"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\ProjetoLive"
!define OBS_EXE "$INSTDIR\OBS\bin\64bit\obs64.exe"
!define PROJECTION_EXE "$INSTDIR\Projecao\FreeShow.exe"
; App "Dispositivos Apple" (traz o serviço que o iPhone usa pelo cabo).
!define APPLE_DEVICES_URL "https://apps.microsoft.com/detail/9np83lwlpz9k"

Name "${PRODUCT}"
OutFile "ProjetoLive-Instalador-${VERSION}.exe"
InstallDir "$PROGRAMFILES64\${PRODUCT}"
InstallDirRegKey HKLM "${UNINSTALL_KEY}" "InstallLocation"
RequestExecutionLevel admin
BrandingText "${PRODUCT} ${VERSION}"

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Instalar o ${PRODUCT}"
!define MUI_WELCOMEPAGE_TEXT "Este instalador coloca no computador:$\r$\n$\r$\n  • o OBS do Projeto Live (transmissão, câmeras de celular, tela vertical)$\r$\n  • o app de projeção (letras, Bíblia, vídeos no telão)$\r$\n$\r$\nTambém libera os dois na rede do Windows e cria os atalhos.$\r$\n$\r$\nFeche o OBS e o app de projeção antes de continuar."
!define MUI_FINISHPAGE_RUN "${OBS_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Abrir o OBS do Projeto Live"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "PortugueseBR"

Function .onInit
	${IfNot} ${RunningX64}
		MessageBox MB_ICONSTOP "O ${PRODUCT} precisa do Windows de 64 bits."
		Abort
	${EndIf}
	SetRegView 64
	; Atalhos para todos os usuários do computador.
	SetShellVarContext all
FunctionEnd

Function un.onInit
	SetRegView 64
	SetShellVarContext all
FunctionEnd

; Regra do firewall: entrada liberada só em redes privadas e de domínio (a rede da igreja).
!macro FirewallRule NAME EXE
	nsExec::Exec 'netsh advfirewall firewall delete rule name="${NAME}"'
	Pop $0
	nsExec::Exec 'netsh advfirewall firewall add rule name="${NAME}" dir=in action=allow program="${EXE}" enable=yes profile=private,domain'
	Pop $0
!macroend

Section "Projeto Live" SecMain
	SectionIn RO

	; Versões antigas saem antes (arquivos que não existem mais na versão nova).
	RMDir /r "$INSTDIR\OBS"
	RMDir /r "$INSTDIR\Projecao"

	SetOutPath "$INSTDIR\OBS"
	File /r "stage\obs\*.*"
	SetOutPath "$INSTDIR\Projecao"
	File /r "stage\projecao\*.*"

	; Atalhos (a pasta de trabalho do OBS precisa ser a do obs64.exe).
	CreateDirectory "$SMPROGRAMS\${PRODUCT}"
	SetOutPath "$INSTDIR\OBS\bin\64bit"
	CreateShortcut "$SMPROGRAMS\${PRODUCT}\Projeto Live (OBS).lnk" "${OBS_EXE}"
	CreateShortcut "$DESKTOP\Projeto Live (OBS).lnk" "${OBS_EXE}"
	SetOutPath "$INSTDIR\Projecao"
	CreateShortcut "$SMPROGRAMS\${PRODUCT}\Projeto Live (Projeção).lnk" "${PROJECTION_EXE}"
	CreateShortcut "$DESKTOP\Projeto Live (Projeção).lnk" "${PROJECTION_EXE}"
	CreateShortcut "$SMPROGRAMS\${PRODUCT}\Desinstalar o Projeto Live.lnk" "$INSTDIR\Desinstalar.exe"

	!insertmacro FirewallRule "Projeto Live (OBS)" "${OBS_EXE}"
	!insertmacro FirewallRule "Projeto Live (Projeção)" "${PROJECTION_EXE}"

	WriteUninstaller "$INSTDIR\Desinstalar.exe"
	WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "${PRODUCT}"
	WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
	WriteRegStr HKLM "${UNINSTALL_KEY}" "Publisher" "${PRODUCT}"
	WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
	WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayIcon" "${OBS_EXE}"
	WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Desinstalar.exe"'
	WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
	WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1
SectionEnd

; iPhone pelo cabo: precisa do serviço da Apple ("Apple Mobile Device Service"), que vem com o app
; "Dispositivos Apple" (ou com o iTunes do site da Apple).
Function .onInstSuccess
	nsExec::Exec 'sc query "Apple Mobile Device Service"'
	Pop $0
	${If} $0 != 0
		MessageBox MB_YESNO|MB_ICONINFORMATION "Para usar o iPhone como câmera pelo CABO USB, o Windows precisa do app $\"Dispositivos Apple$\" (da Microsoft Store).$\r$\n$\r$\nPelo Wi-Fi funciona sem ele.$\r$\n$\r$\nAbrir a página para instalar agora?" IDNO skip
		ExecShell "open" "${APPLE_DEVICES_URL}"
	skip:
	${EndIf}
FunctionEnd

Section "Uninstall"
	nsExec::Exec 'netsh advfirewall firewall delete rule name="Projeto Live (OBS)"'
	Pop $0
	nsExec::Exec 'netsh advfirewall firewall delete rule name="Projeto Live (Projeção)"'
	Pop $0

	Delete "$DESKTOP\Projeto Live (OBS).lnk"
	Delete "$DESKTOP\Projeto Live (Projeção).lnk"
	RMDir /r "$SMPROGRAMS\${PRODUCT}"
	RMDir /r "$INSTDIR\OBS"
	RMDir /r "$INSTDIR\Projecao"
	Delete "$INSTDIR\Desinstalar.exe"
	RMDir "$INSTDIR"
	DeleteRegKey HKLM "${UNINSTALL_KEY}"
	; As configurações (cenas, perfis, músicas) ficam na pasta do usuário e não são apagadas.
SectionEnd
