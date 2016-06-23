#!/usr/bin/perl

use Test::More;
use JavaScript::V8;
use Data::Dumper;

use utf8;
use strict;
use warnings;

my $context;

$context = JavaScript::V8::Context->new;
$context->eval('abc');

$context = JavaScript::V8::Context->new;
$context->eval('abc');

ok('Done');

done_testing;
