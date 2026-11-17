echo @echo off
echo cd /d "%%~dp0"
echo python -m streamlit run dashboard/peer_app.py
echo pause
