#!/usr/bin/env perl

use strict;
use warnings;
use utf8;

use Test::More;

use JavaScript::V8;

my $num42 = \42;
my $num0 = \0;

my $v8context = JavaScript::V8::Context->new();
$v8context->bind( f => $num0 );
ok($v8context->eval('(function() { return f; })()') == 0, 'Testing \0 - should return 0');
$v8context->bind( f => $num42 );
ok($v8context->eval('(function() { return (f === 42 ? 1 : 0) })()') == 1, 'Testing \42 - should return 1');
ok($v8context->eval('typeof f') eq 'number', 'Testing the Javascript type is a number');

done_testing;
