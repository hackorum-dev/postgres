"""-------------------------------------------------------------------------
***
*
* plain_text_mail.py
*   - This tool sends plain text emails.
*
* Created by: Ali Koca
* Copyright (c) 2017-2021, PostgreSQL Global Development Group
*
***-------------------------------------------------------------------------"""

import smtplib, ssl
import os

port = 465
smtp_server = os.getenv("SMTP_SERVER")
sender_email = os.getenv("EMAIL_ADDRESS")

receiver_email = input("Enter receiver email: ")
password = input("Type your password and press enter: ")
message = input("Enter your message: ")

context = ssl.create_default_context()
with smtplib.SMTP_SSL(smtp_server, port, context=context) as server:
    server.login(sender_email, password)
    server.sendmail(sender_email, receiver_email, message)