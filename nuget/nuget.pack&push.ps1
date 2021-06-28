# Function to perform the publication of all components previously packaged in the directory. \ Nupkg for the configured repositories
# in the $Repositories variable
function NugetPush {
  param(    
    [HashTable]$Repositories,
    [String]$AutoPublish
  )

    Write-Output ""
    Write-Output "List of repositories for publication:"
    Write-Output ""
    $Repositories.GetEnumerator() | ForEach-Object -process { $_.Key + " - " + $_.Value }
    Write-Output ""

    if ($AutoPublish -eq "")
    {
      $s = Read-Host -prompt "Do you want to perform the publication (nuget push) of the components packaged above in the repositories listed (y/n)?"
    } else 
    {
      $s = $AutoPublish.ToLower()
    }

    if ($s -eq "y") { 
      Write-Output "Publishing projects to repositories" | Tee-Object .\NugetPush.log
      foreach ($repo in $Repositories.Keys) {
        $RepoURL = $Repositories[$repo][0]
        $ApiKey = $Repositories[$repo][1]
        Write-Output "Publishing projects in $RepoURL (note: if version already exists an error will be logged)" | Tee-Object .\NugetPush.log -Append
        nuget push -Source "$RepoURL" -ApiKey $ApiKey -SkipDuplicate *.nupkg >> NugetPush.log
      }  
      Write-Output "Published" | Tee-Object .\NugetPush.log -Append
      Get-Content .\NugetPush.log 
    }
}

# In order to function it's necessary to set current dir to same location of script 
Set-Location (Split-Path $MyInvocation.MyCommand.Path)

# Save current dir
$CurrentDir = Get-Location

# Start Developer Shell Powershell in order to compile SumatraPDF 
$vsPath = &(Join-Path ${env:ProgramFiles(x86)} "\Microsoft Visual Studio\Installer\vswhere.exe") -property installationpath
. "$vsPath\Common7\Tools\Launch-VsDevShell.ps1"

# Restore current dir
Set-Location $CurrentDir

$s = Read-Host -prompt "Do you want to recompile SumatraPDF.exe (x86/x64) (y/n)?"

# SumatraPDF base dir (git cloned from https://github.com/marcoscmonteiro/sumatrapdf)
$SumatraPDFBaseDir =  ".."

if ($s.ToLower() -eq "y") {
    # Compile SumatraPDF (x64 and Win32 plataform)
    msbuild "$SumatraPDFBaseDir\vs2019\SumatraPDF.vcxproj" /p:Configuration=Release /p:Platform=x64
    msbuild "$SumatraPDFBaseDir\vs2019\SumatraPDF.vcxproj" /p:Configuration=Release /p:Platform=Win32
}

nuget pack .\SumatraPDF.PluginMode.x64.nuspec
nuget pack .\SumatraPDF.PluginMode.x86.nuspec

# Get ApiKey from secret file (not versioned on GIT)
$NugetOrgApiKey = Get-Content ~/Onedrive/Documentos/nuget/NUGET.ORG.APIKEY.TXT

# HashTable containing the repositories with URL and ApiKey for publishing the components
$Repositories = @{
    "Nuget.Org" = @( "https://api.nuget.org/v3/index.json", $NugetOrgApiKey )
}

NugetPush -Repositories $Repositories -AutoPublish ""
