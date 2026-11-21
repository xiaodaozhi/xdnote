# Keep the redirected streams readable while the owned GUI process is running.
# Event handlers are implemented in C# so they do not require a PowerShell runspace.
Add-Type @'
using System;
using System.Diagnostics;
using System.IO;
using System.Text;

public sealed class XdnoteCapturedProcess : IDisposable {
    private readonly object gate = new object();
    private readonly StringBuilder errorText = new StringBuilder();
    private readonly StreamWriter errorFile;
    private readonly StreamWriter outputFile;
    private string failure;
    public Process Process { get; private set; }

    private XdnoteCapturedProcess(string executable, string arguments, string directory,
                                  string errorPath, string outputPath) {
        errorFile = new StreamWriter(new FileStream(errorPath, FileMode.CreateNew,
            FileAccess.Write, FileShare.Read), new UTF8Encoding(false));
        outputFile = new StreamWriter(new FileStream(outputPath, FileMode.CreateNew,
            FileAccess.Write, FileShare.Read), new UTF8Encoding(false));
        errorFile.AutoFlush = true;
        outputFile.AutoFlush = true;
        Process = new Process();
        Process.StartInfo = new ProcessStartInfo {
            FileName = executable, Arguments = arguments, WorkingDirectory = directory,
            UseShellExecute = false, RedirectStandardError = true, RedirectStandardOutput = true
        };
        Process.ErrorDataReceived += delegate(object sender, DataReceivedEventArgs e) {
            Record(e.Data, true);
        };
        Process.OutputDataReceived += delegate(object sender, DataReceivedEventArgs e) {
            Record(e.Data, false);
        };
        if (!Process.Start()) throw new InvalidOperationException("Could not start the owned process.");
        Process.BeginErrorReadLine();
        Process.BeginOutputReadLine();
    }

    public static XdnoteCapturedProcess Start(string executable, string arguments, string directory,
                                              string errorPath, string outputPath) {
        return new XdnoteCapturedProcess(executable, arguments, directory, errorPath, outputPath);
    }

    private void Record(string line, bool isError) {
        if (line == null) return;
        lock (gate) {
            try {
                if (isError) {
                    if (errorText.Length + line.Length + 1 > 4 * 1024 * 1024)
                        throw new InvalidOperationException("The input log exceeded its bounded capture.");
                    errorText.AppendLine(line);
                    errorFile.WriteLine(line);
                } else {
                    outputFile.WriteLine(line);
                }
            } catch (Exception e) {
                if (failure == null) failure = e.Message;
            }
        }
    }

    public string InputLog() {
        lock (gate) {
            if (failure != null) throw new InvalidOperationException("Stream capture failed: " + failure);
            return errorText.ToString();
        }
    }

    public void Dispose() {
        lock (gate) {
            errorFile.Dispose();
            outputFile.Dispose();
        }
        Process.Dispose();
    }
}
'@
