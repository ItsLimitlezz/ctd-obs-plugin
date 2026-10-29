; The Windows installer (SPEC B2). .github/scripts/Package-Windows.ps1 builds it with Inno Setup 6 when ISCC is there,
; passing ProductName, ProductVersion, SourceDir (the plugin folder under release/<config>), OutputDir and OutputBaseName.
; It installs where OBS 28 and later look for plugins for every user: %ProgramData%\obs-studio\plugins\<name>.

#ifndef ProductName
  #define ProductName "ctd-live-source"
#endif
#ifndef ProductVersion
  #define ProductVersion "0.0.0"
#endif

[Setup]
AppId={{6F3B2C1E-5D4A-4E8B-9C7D-2A1F0E3B4C5D}
AppName=CTD OBS Plugin
AppVersion={#ProductVersion}
AppPublisher=CTD
AppPublisherURL=https://github.com/ItsLimitlezz/ctd-obs-plugin
DefaultDirName={commonappdata}\obs-studio\plugins\{#ProductName}
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=CTD OBS Plugin {#ProductVersion}
CloseApplications=yes
CloseApplicationsFilter=obs64.exe

[Messages]
WelcomeLabel2=This installs the CTD OBS Plugin [ver] (CTD Live Source, CTD Hub, CTD Relay) for every user of this computer.%n%nClose OBS before you continue.

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
