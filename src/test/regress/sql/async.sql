--
-- ASYNC
--

--Should work. Send a valid message via a valid channel name
SELECT pg_notify('notify_async1','sample message1');
SELECT pg_notify('notify_async1','');
SELECT pg_notify('notify_async1',NULL);

-- Should fail. Send a valid message via an invalid channel name
SELECT pg_notify('','sample message1');
SELECT pg_notify(NULL,'sample message1');
SELECT pg_notify('notify_async_channel_name_too_long______________________________','sample_message1');

--Should work. Valid NOTIFY/LISTEN/UNLISTEN commands
NOTIFY notify_async2;
LISTEN notify_async2;
SELECT pg_listening_channels(); -- expect one entry
UNLISTEN notify_async2;
--Should work. Ignore unlistened channels
UNLISTEN notify_async2; -- no-op
--Should work. Ignore unlisten all with no channels
UNLISTEN *;

--Should work. Allow listening on everything
LISTEN *;
LISTEN *;
UNLISTEN notify_async3;
LISTEN notify_async3;
UNLISTEN *;

-- Should return zero while there are no pending notifications.
-- src/test/isolation/specs/async-notify.spec tests for actual usage.
SELECT pg_notification_queue_usage();
