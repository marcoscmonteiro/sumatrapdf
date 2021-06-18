## SumatraPDF Reader in enhanced Plugin Mode

About this project:

* It's based on [SumatraPDF](https://www.sumatrapdfreader.org) which is a multi-format (PDF, EPUB, MOBI, FB2, CHM, XPS, DjVu) reader
for Windows under (A)GPLv3 license.

* It's a modified version of original [SumatraPDF](https://www.sumatrapdfreader.org) aimed to work in enhanced plugin mode so it can be used and embeded in 
[SumatraPDFControl](https://sumatrapdfcontrol.mcmonteiro.net).

* [SumatraPDFControl](https://sumatrapdfcontrol.mcmonteiro.net) is a windows Forms Control based on [SumatraPDFReader](https://www.sumatrapdfreader.org/)
to view and read Portable Document Files (PDF)

* This project generates 2 NuGet packages referenced by [SumatraPDFControl](https://sumatrapdfcontrol.mcmonteiro.net):

	* [SumatraPDF PluginMode x86 compiled](https://www.nuget.org/packages/SumatraPDF.PluginMode.x86/)
	* [SumatraPDF PluginMode x64 compiled](https://www.nuget.org/packages/SumatraPDF.PluginMode.x64/)

* To compile you need Visual Studio 2019 16.6 or later. [Free Community edition](https://www.visualstudio.com/vs/community/) works.

	* Open `vs2019/SumatraPDF.sln` and hit F5 to compile and run.

	* For best results use the latest release available as that's what I use and test with.
	If things don't compile, first make sure you're using the latest version of Visual Studio.
