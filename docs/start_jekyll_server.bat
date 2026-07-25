@echo off
chcp 932 >nul
rem ============================================================
rem  ローカルプレビューサーバー
rem  http://127.0.0.1:4000/
rem  Markdown を保存すると自動で再ビルドされる (ブラウザは手動リロード)。
rem  終了はこのウィンドウで Ctrl+C。
rem
rem  必要環境 (初回のみ): Ruby + DevKit
rem    winget install RubyInstallerTeam.RubyWithDevKit.3.2
rem ============================================================
cd /d "%~dp0"

where bundle >nul 2>nul
if errorlevel 1 (
  echo Ruby ^(bundler^) が見つかりません。以下コマンドでインストールしてください:
  echo   winget install RubyInstallerTeam.RubyWithDevKit.3.2
  pause
  exit /b 1
)

call bundle check >nul 2>nul
if errorlevel 1 (
  echo gem をインストールしています ^(数分かかります^)...
  call bundle install
  if errorlevel 1 (
    echo bundle install に失敗しました。
    pause
    exit /b 1
  )
)

rem start "" "http://127.0.0.1:4000/"
call bundle exec jekyll serve --force_polling --baseurl ""
pause


