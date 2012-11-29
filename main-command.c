/* main-command.c
 * Written by Daniel Foote.
 * Started Feb 2005.
 *
 * Command line program to match GPS data and Photo EXIF timestamps
 * together, to figure out where you were at the time.
 * Writes the output back into the GPS exif tags.
 */

/* Copyright 2005 Daniel Foote.
 *
 * This file is part of gpscorrelate.
 *
 * gpscorrelate is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * gpscorrelate is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with gpscorrelate; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <getopt.h>
#include <string.h>
#include <locale.h>

#ifndef WIN32
  #include <termios.h>
#endif

#include "i18n.h"
#include "gpsstructure.h"
#include "exif-gps.h"
#include "unixtime.h"
#include "gpx-read.h"
#include "correlate.h"

/* Command line options structure. */
static const struct option program_options[] = {
	{ "gps", required_argument, 0, 'g' },
	{ "timeadd", required_argument, 0, 'z'},
	{ "no-interpolation", no_argument, 0, 'i'},
	{ "help", no_argument, 0, 'h'},
	{ "verbose", no_argument, 0, 'v'},
	{ "datum", required_argument, 0, 'd'},
	{ "no-write", no_argument, 0, 'n'},
	{ "max-dist", required_argument, 0, 'm'},
	{ "show", no_argument, 0, 's'},
	{ "machine", no_argument, 0, 'o'},
	{ "remove", no_argument, 0, 'r'},
	{ "ignore-tracksegs", no_argument, 0, 't'},
	{ "no-mtime", no_argument, 0, 'M'},
	{ "version", no_argument, 0, 'V'},
	{ "fix-datestamps", no_argument, 0, 'f'},
	{ "degmins", no_argument, 0, 'p'},
	{ "photooffset", required_argument, 0, 'O'},
	{ 0, 0, 0, 0 }
};

/* Function to print the version - near the top for easy modification. */
static void PrintVersion(const char* ProgramName)
{
	printf(_("%s, ver. %s. Daniel Foote, et. al. 2005-2012. GNU GPL.\n"), ProgramName, PACKAGE_VERSION);
}

/* Function to print the usage info. */
static void PrintUsage(const char* ProgramName)
{
	printf(_("Usage: %s [options] file.jpg ...\n"), ProgramName);
	puts(  _("-g, --gps file.gpx       Specifies GPX file with GPS data"));
	puts(  _("-z, --timeadd +/-HH[:MM] Time to add to GPS data to make it match photos"));
	puts(  _("-i, --no-interpolation   Disable interpolation between points; interpolation\n"
	         "                         is linear, points rounded if disabled"));
	puts(  _("-d, --datum DATUM        Specify measurement datum (defaults to WGS-84)"));
	puts(  _("-n, --no-write           Do not write the EXIF data. Useful with --verbose"));
	puts(  _("-m, --max-dist SECS      Max time outside points that photo will be matched"));
	puts(  _("-s, --show               Just show the GPS data from the given files"));
	puts(  _("-o, --machine            Similar to --show but with machine-readable output"));
	puts(  _("-r, --remove             Strip GPS tags from the given files"));
	puts(  _("-t, --ignore-tracksegs   Interpolate between track segments, too"));
	puts(  _("-M, --no-mtime           Don't change mtime of modified files"));
	puts(  _("-f, --fix-datestamps     Fix broken GPS datestamps written with ver. < 1.5.2"));
	puts(  _("    --degmins            Write location as DD MM.MM (was default before v1.5.3)"));
	puts(  _("-O, --photooffset SECS   Offset added to photo time to make it match the GPS"));
	puts(  _("-h, --help               Display usage/help message"));
	puts(  _("-v, --verbose            Show more detailed output"));
	puts(  _("-V, --version            Display version information"));
}

/* Display the information from an existing file. */
static int ShowFileDetails(const char* File, int MachineReadable)
{
	double Lat, Long, Elev;
	int IncludesGPS = 0;
	Lat = Long = Elev = 0;
	char* Time = ReadExifData(File, &Lat, &Long, &Elev, &IncludesGPS);
	int rc = 1;

	if (Time)
	{
		if (IncludesGPS)
		{
			/* Display the data as CSV if we want
			 * it machine readable. */
			if (MachineReadable)
			{
				printf("\"%s\",\"%s\",%f,%f,%.3f\n",
					File, Time, Lat, Long, Elev);
			} else {
				printf(_("%s: %s, Lat %f, Long %f, Elevation %.3f.\n"),
					File, Time, Lat, Long, Elev);
			}
		} else {
			/* Don't display anything if we want machine
			 * readable data and there is no data. */
			if (!MachineReadable)
			{
				printf(_("%s: %s, No GPS Data.\n"),
					File, Time);
			}
		}
	} else {
		/* Say that there was no data, except if we want
		 * machine readable output */
		if (!MachineReadable)
		{
			printf(_("%s: No EXIF data.\n"), File);
			rc = 0;
		}
	}

	free(Time);
	return rc;
}
			
/* Remove all GPS exif tags from a file. Not really that useful, but... */
static int RemoveGPSTags(const char* File, int NoChangeMtime)
{
	if (RemoveGPSExif(File, NoChangeMtime))
	{
		printf(_("%s: Removed GPS tags.\n"), File);
		return 1;
	} else {
		printf(_("%s: Tag removal failure.\n"), File);
		return 0;
	}
}

/* Fix GPSDatestamp tags, if they were incorrect, as found with versions
 * earlier than 1.5.2. */
static int FixDatestamp(const char* File, int AdjustmentHours, int AdjustmentMinutes, int NoWriteExif)
{
	/* Read the timestamp data. */
	char DateStamp[12];
	char TimeStamp[12];
	char CombinedTime[24];
	int IncludesGPS = 0;
	char* OriginalDateStamp = NULL;
	int rc = 1;

	OriginalDateStamp = ReadGPSTimestamp(File, DateStamp, TimeStamp, &IncludesGPS);

	if (OriginalDateStamp == NULL)
	{
		printf(_("%s: No EXIF data.\n"), File);
		rc = 0;
	} else if (IncludesGPS == 0) {
		printf(_("%s: No GPS data.\n"), File);
		rc = 0;
	} else {
		/* Check the timestamp. */
		time_t PhotoTime = ConvertToUnixTime(OriginalDateStamp, EXIF_DATE_FORMAT,
			AdjustmentHours, AdjustmentMinutes);
		
		snprintf(CombinedTime, sizeof(CombinedTime), "%s %s", DateStamp, TimeStamp);

		time_t GPSTime = ConvertToUnixTime(CombinedTime, EXIF_DATE_FORMAT, 0, 0);

		if (PhotoTime != GPSTime) {
			/* Timestamp is wrong. Fix it.
			 * Should be photo time - this also corrects
			 * GPSTimestamp, which was wrong too. */
			if (!NoWriteExif)
			{
				WriteFixedDatestamp(File, PhotoTime);
			}
			char PhotoTimeFormat[100];
			char GPSTimeFormat[100];

			strftime(PhotoTimeFormat, sizeof(PhotoTimeFormat),
				 "%a %b %e %T %Y\n", localtime(&PhotoTime));
			strftime(GPSTimeFormat, sizeof(GPSTimeFormat),
				 "%a %b %e %T %Y\n", localtime(&GPSTime));
			printf(_("%s: Wrong timestamp:\n   Photo:     %s   GPS:       %s   Corrected: %s"),
					File, PhotoTimeFormat, GPSTimeFormat, PhotoTimeFormat);
		} else {
			/* Inside the range. Do nothing! */
			printf(_("%s: Timestamp is OK: Photo %s (localtime), GPS %s (UTC).\n"),
			       File, OriginalDateStamp, CombinedTime);
		}
	}

	free(OriginalDateStamp);
	return rc;
}

int main(int argc, char** argv)
{
	/* Initialize locale & gettext */
	setlocale (LC_ALL, "");
	textdomain(TEXTDOMAIN);

	/* If you didn't pass any arguments... */
	if (argc == 1)
	{
		PrintVersion(argv[0]);
		PrintUsage(argv[0]);
		exit(EXIT_SUCCESS);
	}

	/* Parse our command line options. */
	/* But first, some variables to store stuff in. */
	int c;
	
	struct GPSTrack* Track = NULL;/* Array of lists of GPS waypoints. The
					 final entry of all 0 signals the end. */
	int NumTracks = 0;	     /* Number of track structures at Track,
					not including the terminating entry. */
	int HaveTimeAdjustment = 0;  /* Whether -z option was given. */
	int TimeZoneHours = 0;       /* Integer version of the timezone. */
	int TimeZoneMins = 0;
	char* Datum = NULL;          /* Datum of input GPS data. */
	int Interpolate = 1;         /* Do we interpolate? By default, yes. */
	int NoWriteExif = 0;         /* Do we not write to file? By default, no. */
	int ShowDetails = 0;         /* Do we show lots of details? By default, no. */
	int FeatherTime = 0;         /* The "feather" time, in seconds. 0 = disabled. */
	int ShowOnlyDetails = 0;
	int MachineReadable = 0;
	int RemoveTags = 0;
	int DoBetweenTrackSegs = 0;
	int NoChangeMtime = 0;
	int FixDatestamps = 0;
	int DegMinSecs = 1;
	int PhotoOffset = 0;
	int HaveTrack = 0;

	/* Create the empty terminating array entry */
	Track = (struct GPSTrack*) calloc(1, sizeof(*Track));
	if (!Track)
	{
		printf(_("Out of memory\n"));
		exit(EXIT_FAILURE);
	}

	while (1)
	{
		/* Call getopt to do all the hard work
		 * for us... */
		c = getopt_long(argc, argv, "g:z:ihvd:m:nsortMVfO:",
				program_options, 0);

		if (c == -1) break;

		/* Determine what getopt saw. */
		switch (c)
		{
			case 'g':
				/* This parameter specifies the GPS data.
				 * It must be present at least once. */
				if (optarg)
				{
					/* Read the XML file into memory and extract the "points". */
					printf(_("Reading GPS Data..."));
					fflush(stdout);
					HaveTrack = ReadGPX(optarg, &Track[NumTracks]);
					printf("\n");
					if (!HaveTrack)
					{
						exit(EXIT_FAILURE);
					}

					/* Make room for a new end-of-array entry */
					++NumTracks;
					Track = (struct GPSTrack*) realloc(Track, sizeof(*Track)*(NumTracks+1));
					if (!Track)
					{
						printf(_("Out of memory\n"));
						exit(EXIT_FAILURE);
					}
					memset(&Track[NumTracks], 0, sizeof(*Track));
				}
				break;
			case 'z':
				/* This parameter specifies the time to add to the
				 * GPS data to make it match the timezone for
				 * the photos. */
				/* We only store it here, convert it to numbers later. */
				if (optarg)
				{
					/* Break up the adjustment and convert to numbers. */
					if (strstr(optarg, ":"))
					{
						/* Found colon. Split into two. */
						sscanf(optarg, "%d:%d", &TimeZoneHours, &TimeZoneMins);
						if (TimeZoneHours < 0)
						    TimeZoneMins *= -1;
					} else {
						/* No colon. Just parse. */
						TimeZoneHours = atoi(optarg);
					}
					HaveTimeAdjustment = 1;
				}
				break;
			case 'O':
				PhotoOffset = atoi(optarg);
				break;
			case 'i':
				/* This option disables interpolation. */
				Interpolate = 0;
				break;
			case 'v':
				/* This option asks us to show more info. */
#ifdef WIN32
				printf(_("--verbose does not work on Windows\n"));
#else
				PrintVersion(argv[0]);
				ShowDetails = 1;
#endif
				break;
			case 'd':
				/* This option specifies a Datum, if other than WGS-84. */
				if (optarg)
				{
					Datum = strdup(optarg);
				}
				break;
			case 'n':
				/* This option specifies not to write to file. */
				NoWriteExif = 1;
				break;
			case 'm':
				/* This option gives us the allowable "feather" time. */
				FeatherTime = atoi(optarg);
				break;
			case 'h':
				/* Display the help/usage information. And then quit. */
				PrintUsage(argv[0]);
				exit(EXIT_SUCCESS);
				break;
			case 'V':
				/* Display version information, and then quit. */
				PrintVersion(argv[0]);
				exit(EXIT_SUCCESS);
				break;
			case 'f':
				/* Fix Datestamps. */
				FixDatestamps = 1;
				break;
			case 's':
				/* Show the data in the photos. Mark this. */
				ShowOnlyDetails = 1;
				break;
			case 'o':
				/* Show the data in the photos, machine readable. */
				ShowOnlyDetails = 1;
				MachineReadable = 1;
				break;
			case 'r':
				/* Remove GPS tags from the file. Mark this. */
				RemoveTags = 1;
				break;
			case 't':
				/* Interpolate between track segments. */
				DoBetweenTrackSegs = 1;
				break;
			case 'M':
				NoChangeMtime = 1;
				break;
			case 'p':
				/* Write in old DegMins format. */
				DegMinSecs = 0;
				break;
			case '?':
				/* Unrecognised option. Or, missing argument. */
				/* We should inform the user and let them correct this. */
				printf(_("Next time, please pass a parameter with that!\n"));
				exit(EXIT_FAILURE);
				break;
			default:
				/* Unrecognised code that getopt returned anyway.
				 * Oops... */
				break;
		} /* End switch(c) */
	} /* End While(1) */
	
	/* Check to see if the user passed some files work with. Not much
	 * good if they didn't. */
	if (optind < argc)
	{
		/* You passed some files. Handy! */
	} else {
		/* Hmm. It seems there were no other files... that doesn't work. */
		printf(_("Nice try! However, next time, pass a few JPEG files to match!\n"));
		exit(EXIT_SUCCESS);
	}

	/* If we only wanted to display info on the passed photos, do so now. */
	if (ShowOnlyDetails)
	{
		int result = 1;
		while (optind < argc)
		{
			result = ShowFileDetails(argv[optind++], MachineReadable) && result;
		}
		exit(result ? EXIT_SUCCESS : EXIT_FAILURE);
	}

	/* If we wanted to delete tags, do this now. */
	if (RemoveTags)
	{
		int result = 1;
		while (optind < argc)
		{
			result = RemoveGPSTags(argv[optind++], NoChangeMtime) && result;
		}
		exit(result ? EXIT_SUCCESS : EXIT_FAILURE);
	}

	/* If we wanted to fix datestamps, do this now.
	 * This is to fix incorrect GPSDateStamp values written by versions
	 * earlier than 1.5.2. */
	if (FixDatestamps)
	{
		if (!HaveTimeAdjustment)
		{
			printf(_("You must give a time adjustment for the photos with -z to fix photos.\n"));
			exit(EXIT_FAILURE);
		}
		
		int result = 1;
		while (optind < argc)
		{
			result = FixDatestamp(argv[optind++], TimeZoneHours, TimeZoneMins, NoWriteExif) && result;
		}
		exit(result ? EXIT_SUCCESS : EXIT_FAILURE);
	}

	/* Set up any other command line options... */
	if (!Datum)
	{
		Datum = strdup("WGS-84");
	}

	if (!HaveTrack)
	{
		/* GPS Data was not read correctly... */
		/* Tell the user we are bailing.
		 * Not really required, seeing as ReadGPX should
		 * inform the user anyway... but, doesn't hurt! */
		printf(_("Cannot continue since no GPS data is available.\n"));
		exit(EXIT_FAILURE);
	}

	/* Print a legend for the matching process.
	 * If we're not being verbose. Otherwise, this would be pointless. */
	if (!ShowDetails)
	{
		printf(_("Legend: . = Ok, / = Interpolated, < = Rounded, - = No match, ^ = Too far.\n"
			 "        w = Write Fail, ? = No EXIF date, ! = GPS already present.\n"));
	}

	/* Set up our options structure for the correlation function. */
	struct CorrelateOptions Options;
	Options.NoWriteExif   = NoWriteExif;
	Options.NoInterpolate = (Interpolate ? 0 : 1);
	Options.AutoTimeZone  = !HaveTimeAdjustment;
	Options.TimeZoneHours = TimeZoneHours;
	Options.TimeZoneMins  = TimeZoneMins;
	Options.FeatherTime   = FeatherTime;
	Options.Datum         = Datum;
	Options.DoBetweenTrkSeg = DoBetweenTrackSegs;
	Options.NoChangeMtime = NoChangeMtime;
	Options.DegMinSecs    = DegMinSecs;
	Options.PhotoOffset   = PhotoOffset;

	Options.Track         = Track;

	/* Twig with the terminal settings to make the single character
	 * output stuff work right. */
#ifndef WIN32
	struct termios initial_settings, new_settings;
	if (!ShowDetails)
	{
		tcgetattr(fileno(stdout),&initial_settings);
		new_settings = initial_settings;
		new_settings.c_lflag &= ~ICANON;
		new_settings.c_cc[VMIN] = 1;
		new_settings.c_cc[VTIME] = 0;
		new_settings.c_lflag &= ~ISIG;
		if(tcsetattr(fileno(stdout), TCSANOW, &new_settings) != 0) {
			/* Oops. Oh well. Didn't work. */
			printf(_("Debug: Broken tty set.\n"));
		}
	}
#endif

	/* Make it all look nice and pretty... so the user knows what's going on. */
	printf(_("\nCorrelate: "));
	if (ShowDetails) printf("\n");
	
	/* A few variables that we'll require later. */
	struct GPSPoint* Result;
	char* File;
	/* Including stats on what happened. */
	int MatchExact = 0;
	int MatchInter = 0;
	int MatchRound = 0;
	int NotMatched = 0;
	int WriteFail  = 0;
	int TooFar     = 0;
	int NoDate     = 0;
	int GPSPresent = 0;

	/* Now it is time to correlate the photos. Feed one in at a time, and
	 * see what happens.*/
	/* We already checked to make sure that files were passed on the
	 * command line, so just go for it... */
	/* printf("Remaining non-option arguments: %d.\n", argc - optind); */
	while (optind < argc)
	{
		File = argv[optind++];
		/* Pass the file along to Correlate and see what happens. */
		Result = CorrelatePhoto(File, &Options);

		/* Was result NULL? */
		if (Result)
		{
			/* Result not null. But what did happen? */
			if (Options.Result == CORR_OK)
			{
				MatchExact++;
				if (ShowDetails)
				{
					printf(_("%s: Exact match: "), File);
				} else {
					printf(".");
				}
			}
			if (Options.Result == CORR_INTERPOLATED)
			{
				MatchInter++;
				if (ShowDetails)
				{
					printf(_("%s: Interpolated: "), File);
				} else {
					printf("/");
				}
			}
			if (Options.Result == CORR_ROUND)
			{
				MatchRound++;
				if (ShowDetails)
				{
					printf(_("%s: Rounded: "), File);
				} else {
					printf("<");
				}
			}
			if (Options.Result == CORR_EXIFWRITEFAIL)
			{
				WriteFail++;
				if (ShowDetails)
				{
					printf(_("%s: Exif write failure: "), File);
				} else {
					printf("w");
				}
			}
			if (ShowDetails)
			{
				/* Print out the "point". */
				printf(_("Lat %f, Long %f, Elev %.3f.\n"),
					Result->Lat, Result->Long,
					Result->Elev);
			}
			free(Result);
			/* Ok, that's all from this part... */
		} else {
			/* We got nothing back. One of a few errors. */
			if (Options.Result == CORR_NOMATCH)
			{
				NotMatched++;
				if (ShowDetails)
				{
					printf(_("%s: No match.\n"), File);
				} else {
					printf("-");
				}
			}
			if (Options.Result == CORR_TOOFAR)
			{
				TooFar++;
				if (ShowDetails)
				{
					printf(_("%s: Too far from nearest point.\n"), File);
				} else {
					printf("^");
				}
			}
			if (Options.Result == CORR_NOEXIFINPUT)
			{
				NoDate++;
				if (ShowDetails)
				{
					printf(_("%s: No date exif tag present.\n"), File);
				} else {
					printf("?");
				}
			}
			if (Options.Result == CORR_GPSDATAEXISTS)
			{
				GPSPresent++;
				if (ShowDetails)
				{
					printf(_("%s: GPS Data already present.\n"), File);
				} else {
					printf("!");
				}
			}
			/* Handled all those errors, now... */
		} /* End if Result. */

		/* And, once we've got here, we've finished with that file.
		 * We can now do the next one. Now wasn't that too easy? */
		
	} /* End while parse command line files. */
	
	/* Right, so now we're done. That really wasn't that hard. Right? */

	/* Add a new line if we were doing the not-show-details thing. */
#ifndef WIN32
	if (!ShowDetails)
	{
		printf("\n");
		tcsetattr(fileno(stdout), TCSANOW, &initial_settings);
	}
#endif

	/* Print details of what happened. */
	printf(_("\nCompleted correlation process.\n"));
	if (ShowDetails)
		/* This has to be shown at the end in case auto time zone
		 * was used, since it isn't known before the first file
		 * is processed. */
		printf(_("Used time zone offset %d:%02d\n"),
		       Options.TimeZoneHours, Options.TimeZoneMins);
	printf(_("Matched: %5d (%d Exact, %d Interpolated, %d Rounded).\n"),
			MatchExact + MatchInter + MatchRound,
			MatchExact, MatchInter, MatchRound);
	printf(_("Failed:  %5d (%d Not matched, %d Write failure, %d Too Far,\n"),
			NotMatched + WriteFail + TooFar + NoDate + GPSPresent,
			NotMatched, WriteFail, TooFar);
	printf(_("                %d No Date, %d GPS Already Present.)\n"),
			NoDate, GPSPresent);


	/* Clean up! */
	while (NumTracks > 0)
	{
		--NumTracks;
		FreeTrack(&Track[NumTracks]);
	}
	free(Track);
	free(Datum);
	
	return(WriteFail ? EXIT_FAILURE : EXIT_SUCCESS);
}

