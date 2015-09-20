#!/usr/bin/perl
use Test::More;
use JavaScript::V8;

my $c = JavaScript::V8::Context->new(time_limit => 1);
$c->eval(q{ for(var i = 1; i; i++) { } });
ok $@, "timed out with error";

{
  like $@, qr/timeout/i;
}

done_testing;
