import * as vscode from 'vscode';
import * as net from 'net';
import { ScriptLoader } from './scriptloader';
import { AutoRunner } from './autorunner';
import { VersionChecker } from './version-checker';

let socket: net.Socket | null = null;
let outputChannel: vscode.OutputChannel | null = null;
let statusBarItem: vscode.StatusBarItem | null = null;

function connectToGame(): Promise<boolean> {
    if (!statusBarItem) {
        initStatusBar();
    }

    return new Promise((resolve) => {
        if (socket) {
            resolve(true);
            return;
        }

        socket = new net.Socket();

        socket.connect(28771, '127.0.0.1', () => {
            statusBarItem!.text = 'KCD2: $(plug) Connected';
            if (!outputChannel) {
                outputChannel = vscode.window.createOutputChannel('Lua Output');
            }
            outputChannel.show();
            resolve(true);
        });

        socket.on('error', (error) => {
            statusBarItem!.text = 'KCD2: $(circle-slash) Error connecting';
            vscode.window.showErrorMessage('Failed to connect to game.');
            socket = null;
            outputChannel?.appendLine('Connection error: ' + error.message);
            resolve(false);
        });

        socket.on('end', () => {
            statusBarItem!.text = 'KCD2: $(debug-disconnect) Disconnected';
            socket = null;
            outputChannel?.appendLine('Disconnected from game.');
        });

        socket.on('data', (data) => {
            const response = data.toString();
            if (response.startsWith('Error')) {
                outputChannel?.appendLine(`[Error]: ${response}`);
                outputChannel?.show();
            } else {
                outputChannel?.appendLine(`[Output]: ${response}`);
            }
        });
    });
}

async function sendScriptPaths(scriptPaths: string[]): Promise<void> {
    return new Promise((resolve, reject) => {
        if (!socket) {
            reject(new Error('No socket connection'));
            return;
        }

        // Convert all paths to Unix-style and join with commas
        const pathsString =
            scriptPaths.map((p) => p.replace(/\\/g, '/')).join(',') + '\0';
        const length = Buffer.byteLength(pathsString);
        const lengthBuffer = Buffer.alloc(4);
        lengthBuffer.writeUInt32LE(length, 0);

        // Send length prefix and paths
        const finalBuffer = Buffer.concat([
            lengthBuffer,
            Buffer.from(pathsString),
        ]);

        socket.write(finalBuffer, (err) => {
            if (err) {
                reject(err);
            } else {
                resolve();
            }
        });
    });
}

async function processAndSendScripts(startupScript?: { filepath: string }) {
    const workspaceFolders = vscode.workspace.workspaceFolders;
    if (!workspaceFolders) {
        vscode.window.showErrorMessage('No workspace folder open');
        return;
    }

    if (!socket && !(await connectToGame())) {
        return;
    }

    if (!outputChannel) {
        outputChannel = vscode.window.createOutputChannel('Lua Output');
    }

    const config = vscode.workspace.getConfiguration('kcd2-lua');
    const shouldResolveDependencies = config.get<boolean>(
        'resolveReloadScriptDependencies',
        true
    );

    try {
        outputChannel.appendLine(
            startupScript
                ? 'Processing single script...'
                : 'Processing workspace scripts...'
        );

        if (startupScript && !shouldResolveDependencies) {
            // If dependency resolution is disabled, just send the single script
            outputChannel.appendLine(`Sending: ${startupScript.filepath}`);
            await sendScriptPaths([startupScript.filepath]);
        } else {
            // Process scripts with dependencies if enabled or running workspace scripts
            const loader = new ScriptLoader(
                workspaceFolders[0].uri.fsPath,
                outputChannel
            );
            const scripts = await loader.processScripts(startupScript);

            if (scripts.length === 0) {
                outputChannel.appendLine(
                    'No startup scripts found in workspace'
                );
                return;
            }

            // Extract just the file paths
            const scriptPaths = scripts.map((script) => script.filepath);

            // Send all paths at once
            outputChannel.appendLine('Sending script paths to game...');
            await sendScriptPaths(scriptPaths);
        }

        outputChannel.appendLine('Finished sending script paths');
    } catch (error) {
        const errorMessage =
            error instanceof Error ? error.message : 'Unknown error';
        vscode.window.showErrorMessage(
            `Failed to process ${
                startupScript ? 'script' : 'workspace scripts'
            }: ${errorMessage}`
        );
    }
}

async function runSingleScript() {
    const editor = vscode.window.activeTextEditor;
    if (!editor) {
        return;
    }

    await processAndSendScripts({
        filepath: editor.document.uri.fsPath,
    });
}

async function runWorkspaceScripts() {
    await processAndSendScripts();
}

function initStatusBar() {
    statusBarItem = vscode.window.createStatusBarItem(
        vscode.StatusBarAlignment.Right,
        100
    );
    statusBarItem.text = 'KCD2: $(debug-disconnect) Disconnected';
    statusBarItem.command = 'kcd2-lua.runWorkspace';
    statusBarItem.tooltip = 'Click to run workspace scripts';
    statusBarItem.show();
}

export async function activate(context: vscode.ExtensionContext) {
    const runCommand = vscode.commands.registerCommand('kcd2-lua.run', () =>
        runSingleScript()
    );
    const runWorkspaceCommand = vscode.commands.registerCommand(
        'kcd2-lua.runWorkspace',
        () => runWorkspaceScripts()
    );

    const versionChecker = new VersionChecker(context);
    await versionChecker.checkForMajorUpdate();

    new AutoRunner(context, runSingleScript);

    context.subscriptions.push(runCommand, runWorkspaceCommand);
}

export function deactivate() {
    if (socket) {
        socket.end();
        socket = null;
    }
}
