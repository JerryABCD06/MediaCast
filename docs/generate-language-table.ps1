# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

param(
    [switch]$Check
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$languageRoot = Join-Path $repoRoot 'lang/zh_CN'
$outputPath = Join-Path $PSScriptRoot '中文界面文案对照表.md'

function Read-JsonObject([string]$path) {
    Get-Content -LiteralPath $path -Raw | ConvertFrom-Json -AsHashtable
}

function Escape-Cell([string]$value) {
    return ($value -replace '\|', '\|' -replace "`r?`n", '<br>')
}

$shell = Read-JsonObject (Join-Path $languageRoot 'shell.json')
$ui = (Read-JsonObject (Join-Path $languageRoot 'ui.json')).strings
$terms = (Read-JsonObject (Join-Path $languageRoot 'terms.json')).strings
$total = $shell.Count + $ui.Count + $terms.Count

$builder = [System.Text.StringBuilder]::new()
[void]$builder.AppendLine('# 中文界面文案对照表')
[void]$builder.AppendLine()
[void]$builder.AppendLine('更新日期：2026-11-12')
[void]$builder.AppendLine()
[void]$builder.AppendLine(
    '> 本表由 `docs/generate-language-table.ps1` 根据当前 `lang/zh_CN/` 自动生成。')
[void]$builder.AppendLine('> 权威数据仍是三个 JSON；修改文案时直接改 JSON，再重新生成本表。')
[void]$builder.AppendLine('>')
[void]$builder.AppendLine(
    "> 当前规模：``shell.json`` $($shell.Count) 键，``ui.json`` $($ui.Count) 键，" +
    "``terms.json`` $($terms.Count) 键，共 $total 键。")
[void]$builder.AppendLine()

function Add-Section([string]$title, [string]$file, $items) {
    [void]$builder.AppendLine("## $title")
    [void]$builder.AppendLine()
    [void]$builder.AppendLine("来源：``$file``，$($items.Count) 键。")
    [void]$builder.AppendLine()
    [void]$builder.AppendLine('| 键名 | 当前中文 | 修改为 |')
    [void]$builder.AppendLine('|---|---|---|')
    foreach ($key in $items.Keys) {
        $value = Escape-Cell ([string]$items[$key])
        [void]$builder.AppendLine("| ``$key`` | $value |  |")
    }
    [void]$builder.AppendLine()
}

Add-Section 'Shell 与托盘' 'lang/zh_CN/shell.json' $shell
Add-Section 'QML 界面' 'lang/zh_CN/ui.json' $ui
Add-Section '公共术语' 'lang/zh_CN/terms.json' $terms

$content = $builder.ToString().TrimEnd() + [Environment]::NewLine
if ($Check) {
    $existing = if (Test-Path -LiteralPath $outputPath) {
        [System.IO.File]::ReadAllText($outputPath)
    } else {
        ''
    }
    if ($existing -ne $content) {
        Write-Error '中文界面文案对照表已过期；请运行 docs/generate-language-table.ps1。'
        exit 1
    }
    Write-Host '中文界面文案对照表与语言文件一致。'
    exit 0
}

[System.IO.File]::WriteAllText(
    $outputPath, $content, [System.Text.UTF8Encoding]::new($false))
Write-Host "已生成 $outputPath（$total 键）。"
