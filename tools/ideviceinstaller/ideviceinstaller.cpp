/**
 * ideviceinstaller -- Manage iPhone/iPod apps
 *
 * Copyright (C) 2010 Nikias Bassen <nikias@gmx.li>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more profile.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 
 * USA
 */

#ifndef __func__
# define __func__ __FUNCTION__
#endif

#ifndef S_ISREG
# define S_ISREG(B) ((B)&_S_IFREG)
#endif
#ifndef S_ISDIR
# define S_ISDIR(B) ((B)&_S_IFDIR)
#endif

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <stdlib.h>
#define _GNU_SOURCE 1
#define __USE_GNU 1
#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <time.h>
#include <libgen.h>
#include <inttypes.h>
#include <limits.h>

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/installation_proxy.h>
#include <libimobiledevice/notification_proxy.h>
#include <libimobiledevice/afc.h>

#include <plist/plist.h>

#include <zip.h>

#ifdef WIN32
# include <windows.h>
# define strdup _strdup
#endif

#include <asprintf.h>

#ifndef PRIu64
# define PRIu64 "llu"
#endif
#ifndef PRIi64
# define PRIi64 "ll"
#endif

const char PKG_PATH[] = "/PublicStaging";
const char APPARCH_PATH[] = "ApplicationArchives";

char *udid = NULL;
char *options = NULL;
char *appid = NULL;

int list_apps_mode = 0;
int install_mode = 0;
int uninstall_mode = 0;
int upgrade_mode = 0;
int list_archives_mode = 0;
int archive_mode = 0;
int restore_mode = 0;
int remove_archive_mode = 0;

char *last_status = NULL;
int wait_for_op_complete = 0;
int notification_expected = 0;
int op_completed = 0;
int err_occured = 0;
int notified = 0;

#ifdef HAVE_LIBIMOBILEDEVICE_1_1
static void notifier(const char *notification, void *unused)
#else
static void notifier(const char *notification)
#endif
{
	/* printf("notification received: %s\n", notification);*/
	notified = 1;
}

#ifdef HAVE_LIBIMOBILEDEVICE_1_1
//static void status_cb(const char *operation, plist_t status, void *unused)
static void status_cb(plist_t operation, plist_t status, void *unused)
#else
static void status_cb(const char *operation, plist_t status)
#endif
{
	if (status && operation) {
		plist_t npercent = plist_dict_get_item(status, "PercentComplete");
		plist_t nstatus = plist_dict_get_item(status, "Status");
		plist_t nerror = plist_dict_get_item(status, "Error");
		int percent = 0;
		char *status_msg = NULL;
		if (npercent) {
			uint64_t val = 0;
			plist_get_uint_val(npercent, &val);
			percent = val;
		}
		if (nstatus) {
			plist_get_string_val(nstatus, &status_msg);
			if (!strcmp(status_msg, "Complete")) {
				op_completed = 1;
			}
		}
		if (!nerror) {
			if (last_status && (strcmp(last_status, status_msg))) {
				printf("\n");
			}

			if (!npercent) {
				//printf("%s - %s\n", operation, status_msg);
				printf("operation - %s\n",  status_msg);
			} else {
				//printf("%s - %s (%d%%)\r", operation, status_msg, percent);
				printf("operation - %s (%d%%)\r",  status_msg, percent);
			}
		} else {
			char *err_msg = NULL;
			plist_get_string_val(nerror, &err_msg);
			//printf("%s - Error occured: %s\n", operation, err_msg);
			printf("operation - Error occured: %s\n",  err_msg);
			free(err_msg);
			err_occured = 1;
		}
		if (last_status) {
			free(last_status);
			last_status = NULL;
		}
		if (status_msg) {
			last_status = strdup(status_msg);
			free(status_msg);
		}
	} else {
		printf("%s: called with invalid data!\n", __func__);
	}
}

static int zip_get_contents(struct zip *zf, const char *filename, int locate_flags, char **buffer, uint32_t *len)
{
	struct zip_stat zs;
	struct zip_file *zfile;
	int zindex = zip_name_locate(zf, filename, locate_flags);

	*buffer = NULL;
	*len = 0;

	if (zindex < 0) {
		fprintf(stderr, "ERROR: could not locate %s in archive!\n", filename);
		return -1;
	}

	zip_stat_init(&zs);

	if (zip_stat_index(zf, zindex, 0, &zs) != 0) {
		fprintf(stderr, "ERROR: zip_stat_index '%s' failed!\n", filename);
		return -2;
	}

	if (zs.size > 10485760) {
		fprintf(stderr, "ERROR: file '%s' is too large!\n", filename);
		return -3;
	}

	zfile = zip_fopen_index(zf, zindex, 0);
	if (!zfile) {
		fprintf(stderr, "ERROR: zip_fopen '%s' failed!\n", filename);
		return -4;
	}

	*buffer = (char *)malloc(zs.size);
	if (zs.size > LLONG_MAX || zip_fread(zfile, *buffer, zs.size) != (zip_int64_t)zs.size) {
		fprintf(stderr, "ERROR: zip_fread %llu bytes from '%s'\n", (uint64_t)zs.size, filename);
		free(*buffer);
		*buffer = NULL;
		zip_fclose(zfile);
		return -5;
	}
	*len = zs.size;
	zip_fclose(zfile);
	return 0;
}

static int zip_get_app_directory(struct zip* zf, char** path)
{
	int i = 0;
	int c = zip_get_num_files(zf);
	int len = 0;
	const char* name = NULL;

	/* look through all filenames in the archive */
	do {
		/* get filename at current index */
		name = zip_get_name(zf, i++, 0);
		if (name != NULL) {
			/* check if we have a "Payload/.../" name */
			len = strlen(name);
			if (!strncmp(name, "Payload/", 8) && (len > 8)) {
				/* locate the second directory delimiter */
				const char* p = name + 8;
				do {
					if (*p == '/') {
						break;
					}
				} while(p++ != NULL);

				/* try next entry if not found */
				if (p == NULL)
					continue;

				len = p - name + 1;

				if (*path != NULL) {
					free(*path);
					*path = NULL;
				}

				/* allocate and copy filename */
				*path = (char*)malloc(len + 1);
				strncpy(*path, name, len);

				/* add terminating null character */
				char* t = *path + len;
				*t = '\0';
				break;
			}
		}
	} while(i < c);

	/* check if the path actually exists */
	int zindex = zip_name_locate(zf, *path, 0);
	if (zindex < 0) {
		return -1;
	}

	return 0;
}

static void do_wait_when_needed()
{
	int i = 0;
#ifndef WIN32
	struct timespec ts;
	ts.tv_sec = 0;
	ts.tv_nsec = 500000000;
#else
	unsigned long sleep_duration = 500;
#endif
	/* wait for operation to complete */
	while (wait_for_op_complete && !op_completed && !err_occured
		   && !notified && (i < 60)) {
#ifndef WIN32
		nanosleep(&ts, NULL);
#else
		Sleep(sleep_duration);
#endif
		i++;
	}

	/* wait some time if a notification is expected */
	while (notification_expected && !notified && !err_occured && (i < 10)) {
#ifndef WIN32
		nanosleep(&ts, NULL);
#else
		Sleep(sleep_duration);
#endif
		i++;
	}
}

static void print_usage(int argc, char **argv)
{
	char *name = NULL;

	name = strrchr(argv[0], '/');
	printf("Usage: %s OPTIONS\n", (name ? name + 1 : argv[0]));
	printf("Manage apps on an iDevice.\n\n");
	printf
		("  -U, --udid UDID\tTarget specific device by its 40-digit device UDID.\n"
		 "  -l, --list-apps\tList apps, possible options:\n"
		 "       -o list_user\t- list user apps only (this is the default)\n"
		 "       -o list_system\t- list system apps only\n"
		 "       -o list_all\t- list all types of apps\n"
		 "       -o xml\t\t- print full output as xml plist\n"
		 "  -i, --install ARCHIVE\tInstall app from package file specified by ARCHIVE.\n"
		 "                       \tARCHIVE can also be a .ipcc file for carrier bundles.\n"
		 "  -u, --uninstall APPID\tUninstall app specified by APPID.\n"
		 "  -g, --upgrade ARCHIVE\tUpgrade app from package file specified by ARCHIVE.\n"
		 "  -L, --list-archives\tList archived applications, possible options:\n"
		 "       -o xml\t\t- print full output as xml plist\n"
		 "  -a, --archive APPID\tArchive app specified by APPID, possible options:\n"
		 "       -o uninstall\t- uninstall the package after making an archive\n"
		 "       -o app_only\t- archive application data only\n"
		 "       -o docs_only\t- archive documents (user data) only\n"
		 "       -o copy=PATH\t- copy the app archive to directory PATH when done\n"
		 "       -o remove\t- only valid when copy=PATH is used: remove after copy\n"
		 "  -r, --restore APPID\tRestore archived app specified by APPID\n"
		 "  -R, --remove-archive APPID  Remove app archive specified by APPID\n"
		 "  -o, --options\t\tPass additional options to the specified command.\n"
		 "  -h, --help\t\tprints usage information\n"
		 "  -d, --debug\t\tenable communication debugging\n" "\n");
}

static void parse_opts(int argc, char **argv)
{
	static struct option longopts[] = {
		{"help", 0, NULL, 'h'},
		{"udid", 1, NULL, 'U'},
		{"list-apps", 0, NULL, 'l'},
		{"install", 1, NULL, 'i'},
		{"uninstall", 1, NULL, 'u'},
		{"upgrade", 1, NULL, 'g'},
		{"list-archives", 0, NULL, 'L'},
		{"archive", 1, NULL, 'a'},
		{"restore", 1, NULL, 'r'},
		{"remove-archive", 1, NULL, 'R'},
		{"options", 1, NULL, 'o'},
		{"debug", 0, NULL, 'd'},
		{NULL, 0, NULL, 0}
	};
	int c;

	while (1) {
		c = getopt_long(argc, argv, "hU:li:u:g:La:r:R:o:d", longopts,
						(int *) 0);
		if (c == -1) {
			break;
		}

		switch (c) {
		case 'h':
			print_usage(argc, argv);
			exit(0);
		case 'U':
			if (strlen(optarg) != 40) {
				printf("%s: invalid UDID specified (length != 40)\n",
					   argv[0]);
				print_usage(argc, argv);
				exit(2);
			}
			udid = strdup(optarg);
			break;
		case 'l':
			list_apps_mode = 1;
			break;
		case 'i':
			install_mode = 1;
			appid = strdup(optarg);
			break;
		case 'u':
			uninstall_mode = 1;
			appid = strdup(optarg);
			break;
		case 'g':
			upgrade_mode = 1;
			appid = strdup(optarg);
			break;
		case 'L':
			list_archives_mode = 1;
			break;
		case 'a':
			archive_mode = 1;
			appid = strdup(optarg);
			break;
		case 'r':
			restore_mode = 1;
			appid = strdup(optarg);
			break;
		case 'R':
			remove_archive_mode = 1;
			appid = strdup(optarg);
			break;
		case 'o':
			if (!options) {
				options = strdup(optarg);
			} else {
				char *newopts =	(char *)malloc(strlen(options) + strlen(optarg) + 2);
				strcpy(newopts, options);
				free(options);
				strcat(newopts, ",");
				strcat(newopts, optarg);
				options = newopts;
			}
			break;
		case 'd':
			idevice_set_debug_level(1);
			break;
		default:
			print_usage(argc, argv);
			exit(2);
		}
	}

	if (optind <= 1 || (argc - optind > 0)) {
		print_usage(argc, argv);
		exit(2);
	}
}

int main(int argc, char **argv)
{
	idevice_t phone = NULL;
	lockdownd_client_t client = NULL;
	instproxy_client_t ipc = NULL;
	np_client_t np = NULL;
	afc_client_t afc = NULL;
	lockdownd_service_descriptor_t service_descriptor = NULL;
	int res = 0;

	parse_opts(argc, argv);

	argc -= optind;
	argv += optind;

	if (IDEVICE_E_SUCCESS != idevice_new(&phone, udid)) {
		fprintf(stderr, "No iPhone found, is it plugged in?\n");
		return -1;
	}

	if (LOCKDOWN_E_SUCCESS != lockdownd_client_new_with_handshake(phone, &client, "ideviceinstaller")) {
		fprintf(stderr, "Could not connect to lockdownd. Exiting.\n");
		goto leave_cleanup;
	}

	if ((lockdownd_start_service
		 (client, "com.apple.mobile.notification_proxy",
		  &service_descriptor) != LOCKDOWN_E_SUCCESS) || !service_descriptor->port) {
		fprintf(stderr,
				"Could not start com.apple.mobile.notification_proxy!\n");
		goto leave_cleanup;
	}

	if (np_client_new(phone, service_descriptor, &np) != NP_E_SUCCESS) {
		fprintf(stderr, "Could not connect to notification_proxy!\n");
		goto leave_cleanup;
	}

#ifdef HAVE_LIBIMOBILEDEVICE_1_1
	np_set_notify_callback(np, notifier, NULL);
#else
	np_set_notify_callback(np, notifier);
#endif

	const char *noties[3] = { NP_APP_INSTALLED, NP_APP_UNINSTALLED, NULL };

	np_observe_notifications(np, noties);

run_again:
	service_descriptor = NULL;
	if ((lockdownd_start_service
		 (client, "com.apple.mobile.installation_proxy",
		  &service_descriptor) != LOCKDOWN_E_SUCCESS) || !service_descriptor->port) {
		fprintf(stderr,
				"Could not start com.apple.mobile.installation_proxy!\n");
		goto leave_cleanup;
	}

	if (instproxy_client_new(phone, service_descriptor, &ipc) != INSTPROXY_E_SUCCESS) {
		fprintf(stderr, "Could not connect to installation_proxy!\n");
		goto leave_cleanup;
	}

	setbuf(stdout, NULL);

	if (last_status) {
		free(last_status);
		last_status = NULL;
	}
	notification_expected = 0;

	if (list_apps_mode) {
		int xml_mode = 0;
		plist_t client_opts = instproxy_client_options_new();
		instproxy_client_options_add(client_opts, "ApplicationType", "User", NULL);
		instproxy_error_t err;
		plist_t apps = NULL;

		/* look for options */
		if (options) {
			char *opts = strdup(options);
			char *elem = strtok(opts, ",");
			while (elem) {
				if (!strcmp(elem, "list_system")) {
					if (!client_opts) {
						client_opts = instproxy_client_options_new();
					}
					instproxy_client_options_add(client_opts, "ApplicationType", "System", NULL);
				} else if (!strcmp(elem, "list_all")) {
					instproxy_client_options_free(client_opts);
					client_opts = NULL;
				} else if (!strcmp(elem, "list_user")) {
					/* do nothing, we're already set */
				} else if (!strcmp(elem, "xml")) {
					xml_mode = 1;
				}
				elem = strtok(NULL, ",");
			}
		}

		err = instproxy_browse(ipc, client_opts, &apps);
		instproxy_client_options_free(client_opts);
		if (err != INSTPROXY_E_SUCCESS) {
			fprintf(stderr, "ERROR: instproxy_browse returned %d\n", err);
			goto leave_cleanup;
		}
		if (!apps || (plist_get_node_type(apps) != PLIST_ARRAY)) {
			fprintf(stderr,
					"ERROR: instproxy_browse returnd an invalid plist!\n");
			goto leave_cleanup;
		}
		if (xml_mode) {
			char *xml = NULL;
			uint32_t len = 0;

			plist_to_xml(apps, &xml, &len);
			if (xml) {
				puts(xml);
				free(xml);
			}
			plist_free(apps);
			goto leave_cleanup;
		}
		printf("Total: %d apps\n", plist_array_get_size(apps));
		uint32_t i = 0;
		for (i = 0; i < plist_array_get_size(apps); i++) {
			plist_t app = plist_array_get_item(apps, i);
			plist_t p_appid =
				plist_dict_get_item(app, "CFBundleIdentifier");
			char *s_appid = NULL;
			char *s_dispName = NULL;
			char *s_version = NULL;
			plist_t dispName =
				plist_dict_get_item(app, "CFBundleDisplayName");
			plist_t version = plist_dict_get_item(app, "CFBundleVersion");

			if (p_appid) {
				plist_get_string_val(p_appid, &s_appid);
			}
			if (!s_appid) {
				fprintf(stderr, "ERROR: Failed to get APPID!\n");
				break;
			}

			if (dispName) {
				plist_get_string_val(dispName, &s_dispName);
			}
			if (version) {
				plist_get_string_val(version, &s_version);
			}

			if (!s_dispName) {
				s_dispName = strdup(s_appid);
			}
			if (s_version) {
				printf("%s - %s %s\n", s_appid, s_dispName, s_version);
				free(s_version);
			} else {
				printf("%s - %s\n", s_appid, s_dispName);
			}
			free(s_dispName);
			free(s_appid);
		}
		plist_free(apps);
	} else if (install_mode || upgrade_mode) {
		plist_t sinf = NULL;
		plist_t meta = NULL;
		char *pkgname = NULL;
		struct stat fst;
		FILE *f = NULL;
		uint64_t af = 0;
		char buf[8192];

		service_descriptor = NULL;
		if ((lockdownd_start_service(client, "com.apple.afc", &service_descriptor) !=
			 LOCKDOWN_E_SUCCESS) || !service_descriptor->port) {
			fprintf(stderr, "Could not start com.apple.afc!\n");
			goto leave_cleanup;
		}

		lockdownd_client_free(client);
		client = NULL;

		if (afc_client_new(phone, service_descriptor, &afc) != INSTPROXY_E_SUCCESS) {
			fprintf(stderr, "Could not connect to AFC!\n");
			goto leave_cleanup;
		}

		if (stat(appid, &fst) != 0) {
			fprintf(stderr, "ERROR: stat: %s: %s\n", appid, strerror(errno));
			goto leave_cleanup;
		}

		/* open install package */
		int errp = 0;
		struct zip *zf = zip_open(appid, 0, &errp);
		if (!zf) {
			fprintf(stderr, "ERROR: zip_open: %s: %d\n", appid, errp);
			goto leave_cleanup;
		}

		plist_t client_opts = instproxy_client_options_new();

		if ((strlen(appid) > 5) && (strcmp(&appid[strlen(appid)-5], ".ipcc") == 0)) {
			char* ipcc = strdup(appid);
			if ((asprintf(&pkgname, "%s/%s", PKG_PATH, basename(ipcc)) > 0) && pkgname) {
				afc_make_directory(afc, pkgname);
			}

			printf("Uploading %s package contents...\n", basename(ipcc));

			/* extract the contents of the .ipcc file to PublicStaging/<name>.ipcc directory */
			zip_uint64_t numzf = zip_get_num_entries(zf, 0);
			zip_uint64_t i = 0;
			for (i = 0; numzf > 0 && i < numzf; i++) {
				const char* zname = zip_get_name(zf, i, 0);
				char* dstpath = NULL;
				if (!zname) continue;
				if (zname[strlen(zname)-1] == '/') {
					// directory
					if ((asprintf(&dstpath, "%s/%s/%s", PKG_PATH, basename(ipcc), zname) > 0) && dstpath) {
						afc_make_directory(afc, dstpath);						}
					free(dstpath);
					dstpath = NULL;
				} else {
					// file
					struct zip_file* zfile = zip_fopen_index(zf, i, 0);
					if (!zfile) continue;

					if ((asprintf(&dstpath, "%s/%s/%s", PKG_PATH, basename(ipcc), zname) <= 0) || !dstpath || (afc_file_open(afc, dstpath, AFC_FOPEN_WRONLY, &af) != AFC_E_SUCCESS)) {
						fprintf(stderr, "ERROR: can't open afc://%s for writing\n", dstpath);
						free(dstpath);
						dstpath = NULL;
						zip_fclose(zfile);
						continue;
					}

					struct zip_stat zs;
					zip_stat_init(&zs);
					if (zip_stat_index(zf, i, 0, &zs) != 0) {
						fprintf(stderr, "ERROR: zip_stat_index %" PRIu64 " failed!\n", i);
						free(dstpath);
						dstpath = NULL;
						zip_fclose(zfile);
						continue;
					}

					free(dstpath);
					dstpath = NULL;

					zip_uint64_t zfsize = 0;
					while (zfsize < zs.size) {
						zip_int64_t amount = zip_fread(zfile, buf, sizeof(buf));
						if (amount == 0) {
							break;
						}

						if (amount > 0) {
							uint32_t written, total = 0;
							while (total < amount) {
								written = 0;
								if (afc_file_write(afc, af, buf, amount, &written) !=
									AFC_E_SUCCESS) {
									fprintf(stderr, "AFC Write error!\n");
									break;
								}
								total += written;
							}
							if (total != amount) {
								fprintf(stderr, "Error: wrote only %d of %" PRIi64 "\n", total, amount);
								afc_file_close(afc, af);
								zip_fclose(zfile);
								free(dstpath);
								goto leave_cleanup;
							}
						}

						zfsize += amount;
					}

					afc_file_close(afc, af);
					af = 0;

					zip_fclose(zfile);
				}
			}
			free(ipcc);
			printf("done.\n");

			instproxy_client_options_add(client_opts, "PackageType", "CarrierBundle", NULL);
		} else {
			/* extract iTunesMetadata.plist from package */
			char *zbuf = NULL;
			uint32_t len = 0;
			plist_t meta_dict = NULL;
			if (zip_get_contents(zf, "iTunesMetadata.plist", 0, &zbuf, &len) == 0) {
				meta = plist_new_data(zbuf, len);
				if (memcmp(zbuf, "bplist00", 8) == 0) {
					plist_from_bin(zbuf, len, &meta_dict);
				} else {
					plist_from_xml(zbuf, len, &meta_dict);
				}
			}
			if (zbuf) {
				free(zbuf);
			}

			/* determine .app directory in archive */
			zbuf = NULL;
			len = 0;
			plist_t info = NULL;
			char filename[256];
			filename[0] = '\0';
			char* app_directory_name = NULL;

			if (zip_get_app_directory(zf, &app_directory_name)) {
				fprintf(stderr, "Unable to locate app directory in archive!\n");
				goto leave_cleanup;
			}

			/* construct full filename to Info.plist */
			strcpy(filename, app_directory_name);
			free(app_directory_name);
			app_directory_name = NULL;
			strcat(filename, "Info.plist");

			if (zip_get_contents(zf, filename, 0, &zbuf, &len) < 0) {
				zip_unchange_all(zf);
				zip_close(zf);
				goto leave_cleanup;
			}
			if (memcmp(zbuf, "bplist00", 8) == 0) {
				plist_from_bin(zbuf, len, &info);
			} else {
				plist_from_xml(zbuf, len, &info);
			}
			free(zbuf);

			if (!info) {
				fprintf(stderr, "Could not parse Info.plist!\n");
				zip_unchange_all(zf);
				zip_close(zf);
				goto leave_cleanup;
			}

			char *bundleexecutable = NULL;

			plist_t bname = plist_dict_get_item(info, "CFBundleExecutable");
			if (bname) {
				plist_get_string_val(bname, &bundleexecutable);
			}
			plist_free(info);

			if (!bundleexecutable) {
				fprintf(stderr, "Could not determine value for CFBundleExecutable!\n");
				zip_unchange_all(zf);
				zip_close(zf);
				goto leave_cleanup;
			}
			char tem[100];
			sprintf(tem, "Payload/%s.app/SC_Info/%s.sinf", bundleexecutable, bundleexecutable);
			
			char *sinfname = (char*)malloc(strlen(tem) + 1);
			sprintf(sinfname, "%s", tem);
			//if (asprintf(&sinfname, "Payload/%s.app/SC_Info/%s.sinf", bundleexecutable, bundleexecutable) < 0) {
			if (!sinfname){
				fprintf(stderr, "Out of memory!?\n");
				goto leave_cleanup;
			}
			free(bundleexecutable);

			/* extract .sinf from package */
			zbuf = NULL;
			len = 0;
			if (zip_get_contents(zf, sinfname, 0, &zbuf, &len) == 0) {
				sinf = plist_new_data(zbuf, len);
			}
			free(sinfname);
			if (zbuf) {
				free(zbuf);
			}

			/* copy archive to device */
			f = fopen(appid, "rb");
			if (!f) {
				fprintf(stderr, "fopen: %s: %s\n", appid, strerror(errno));
				goto leave_cleanup;
			}

			pkgname = NULL;
			sprintf(tem, "%s/%s", PKG_PATH, appid);
			pkgname = (char*)malloc(strlen(tem) + 1);
			sprintf(pkgname, "%s", tem);
		//	if (asprintf(&pkgname, "%s/%s", PKG_PATH, basename(appid)) < 0) {
			if (!pkgname){
				fprintf(stderr, "Out of memory!?\n");
				goto leave_cleanup;
			}

			printf("Copying '%s' --> '%s'\n", appid, pkgname);

			char **strs = NULL;
			if (afc_get_file_info(afc, PKG_PATH, &strs) != AFC_E_SUCCESS) {
				if (afc_make_directory(afc, PKG_PATH) != AFC_E_SUCCESS) {
					fprintf(stderr, "WARNING: Could not create directory '%s' on device!\n", PKG_PATH);
				}
			}
			if (strs) {
				int i = 0;
				while (strs[i]) {
					free(strs[i]);
					i++;
				}
				free(strs);
			}

			if ((afc_file_open(afc, pkgname, AFC_FOPEN_WRONLY, &af) !=
				 AFC_E_SUCCESS) || !af) {
				fclose(f);
				fprintf(stderr, "afc_file_open on '%s' failed!\n", pkgname);
				free(pkgname);
				goto leave_cleanup;
			}

			size_t amount = 0;
			do {
				amount = fread(buf, 1, sizeof(buf), f);
				if (amount > 0) {
					uint32_t written, total = 0;
					while (total < amount) {
						written = 0;
						if (afc_file_write(afc, af, buf, amount, &written) !=
							AFC_E_SUCCESS) {
							fprintf(stderr, "AFC Write error!\n");
							break;
						}
						total += written;
					}
					if (total != amount) {
						fprintf(stderr, "Error: wrote only %d of %zu\n", total,
							amount);
						afc_file_close(afc, af);
						fclose(f);
						free(pkgname);
						goto leave_cleanup;
					}
				}
			}
			while (amount > 0);

			afc_file_close(afc, af);
			fclose(f);

			printf("done.\n");

			if (sinf) {
				instproxy_client_options_add(client_opts, "ApplicationSINF", sinf, NULL);
			}
			if (meta) {
				instproxy_client_options_add(client_opts, "iTunesMetadata", meta, NULL);
			}
		}
		zip_unchange_all(zf);
		zip_close(zf);

		/* perform installation or upgrade */
		if (install_mode) {
			printf("Installing '%s'\n", pkgname);
#ifdef HAVE_LIBIMOBILEDEVICE_1_1
			instproxy_install(ipc, pkgname, client_opts, status_cb, NULL);
#else
			instproxy_install(ipc, pkgname, client_opts, status_cb);
#endif
		} else {
			printf("Upgrading '%s'\n", pkgname);
#ifdef HAVE_LIBIMOBILEDEVICE_1_1
			instproxy_upgrade(ipc, pkgname, client_opts, status_cb, NULL);
#else
			instproxy_upgrade(ipc, pkgname, client_opts, status_cb);
#endif
		}
		instproxy_client_options_free(client_opts);
		free(pkgname);
		wait_for_op_complete = 1;
		notification_expected = 1;
	} else if (uninstall_mode) {
#ifdef HAVE_LIBIMOBILEDEVICE_1_1
		instproxy_uninstall(ipc, appid, NULL, status_cb, NULL);
#else
		instproxy_uninstall(ipc, appid, NULL, status_cb);
#endif
		wait_for_op_complete = 1;
		notification_expected = 1;
	} else if (list_archives_mode) {
		int xml_mode = 0;
		plist_t dict = NULL;
		plist_t lres = NULL;
		instproxy_error_t err;

		/* look for options */
		if (options) {
			char *opts = strdup(options);
			char *elem = strtok(opts, ",");
			while (elem) {
				if (!strcmp(elem, "xml")) {
					xml_mode = 1;
				}
				elem = strtok(NULL, ",");
			}
		}

		err = instproxy_lookup_archives(ipc, NULL, &dict);
		if (err != INSTPROXY_E_SUCCESS) {
			fprintf(stderr, "ERROR: lookup_archives returned %d\n", err);
			goto leave_cleanup;
		}
		if (!dict) {
			fprintf(stderr,
					"ERROR: lookup_archives did not return a plist!?\n");
			goto leave_cleanup;
		}

		lres = plist_dict_get_item(dict, "LookupResult");
		if (!lres || (plist_get_node_type(lres) != PLIST_DICT)) {
			plist_free(dict);
			fprintf(stderr, "ERROR: Could not get dict 'LookupResult'\n");
			goto leave_cleanup;
		}

		if (xml_mode) {
			char *xml = NULL;
			uint32_t len = 0;

			plist_to_xml(lres, &xml, &len);
			if (xml) {
				puts(xml);
				free(xml);
			}
			plist_free(dict);
			goto leave_cleanup;
		}
		plist_dict_iter iter = NULL;
		plist_t node = NULL;
		char *key = NULL;

		printf("Total: %d archived apps\n", plist_dict_get_size(lres));
		plist_dict_new_iter(lres, &iter);
		if (!iter) {
			plist_free(dict);
			fprintf(stderr, "ERROR: Could not create plist_dict_iter!\n");
			goto leave_cleanup;
		}
		do {
			key = NULL;
			node = NULL;
			plist_dict_next_item(lres, iter, &key, &node);
			if (key && (plist_get_node_type(node) == PLIST_DICT)) {
				char *s_dispName = NULL;
				char *s_version = NULL;
				plist_t dispName =
					plist_dict_get_item(node, "CFBundleDisplayName");
				plist_t version =
					plist_dict_get_item(node, "CFBundleVersion");
				if (dispName) {
					plist_get_string_val(dispName, &s_dispName);
				}
				if (version) {
					plist_get_string_val(version, &s_version);
				}
				if (!s_dispName) {
					s_dispName = strdup(key);
				}
				if (s_version) {
					printf("%s - %s %s\n", key, s_dispName, s_version);
					free(s_version);
				} else {
					printf("%s - %s\n", key, s_dispName);
				}
				free(s_dispName);
				free(key);
			}
		}
		while (node);
		plist_free(dict);
	} else if (archive_mode) {
		char *copy_path = NULL;
		int remove_after_copy = 0;
		int skip_uninstall = 1;
		int app_only = 0;
		int docs_only = 0;
		plist_t client_opts = NULL;

		/* look for options */
		if (options) {
			char *opts = strdup(options);
			char *elem = strtok(opts, ",");
			while (elem) {
				if (!strcmp(elem, "uninstall")) {
					skip_uninstall = 0;
				} else if (!strcmp(elem, "app_only")) {
					app_only = 1;
					docs_only = 0;
				} else if (!strcmp(elem, "docs_only")) {
					docs_only = 1;
					app_only = 0;
				} else if ((strlen(elem) > 5) && !strncmp(elem, "copy=", 5)) {
					copy_path = strdup(elem+5);
				} else if (!strcmp(elem, "remove")) {
					remove_after_copy = 1;
				}
				elem = strtok(NULL, ",");
			}
		}

		if (skip_uninstall || app_only || docs_only) {
			client_opts = instproxy_client_options_new();
			if (skip_uninstall) {
				instproxy_client_options_add(client_opts, "SkipUninstall", 1, NULL);
			}
			if (app_only) {
				instproxy_client_options_add(client_opts, "ArchiveType", "ApplicationOnly", NULL);
			} else if (docs_only) {
				instproxy_client_options_add(client_opts, "ArchiveType", "DocumentsOnly", NULL);
			}
		}

		if (copy_path) {
			struct stat fst;
			if (stat(copy_path, &fst) != 0) {
				fprintf(stderr, "ERROR: stat: %s: %s\n", copy_path, strerror(errno));
				free(copy_path);
				goto leave_cleanup;
			}
			
			if (!S_ISDIR(fst.st_mode)) {
				fprintf(stderr, "ERROR: '%s' is not a directory as expected.\n", copy_path);
				free(copy_path);
				goto leave_cleanup;
			}

			service_descriptor = NULL;
			if ((lockdownd_start_service(client, "com.apple.afc", &service_descriptor) != LOCKDOWN_E_SUCCESS) || !service_descriptor->port) {
				fprintf(stderr, "Could not start com.apple.afc!\n");
				free(copy_path);
				goto leave_cleanup;
			}

			lockdownd_client_free(client);
			client = NULL;

			if (afc_client_new(phone, service_descriptor, &afc) != INSTPROXY_E_SUCCESS) {
				fprintf(stderr, "Could not connect to AFC!\n");
				goto leave_cleanup;
			}
		}

#ifdef HAVE_LIBIMOBILEDEVICE_1_1
		instproxy_archive(ipc, appid, client_opts, status_cb, NULL);
#else
		instproxy_archive(ipc, appid, client_opts, status_cb);
#endif
		instproxy_client_options_free(client_opts);
		wait_for_op_complete = 1;
		if (skip_uninstall) {
			notification_expected = 0;
		} else {
			notification_expected = 1;
		}

		do_wait_when_needed();

		if (copy_path) {
			if (err_occured) {
				afc_client_free(afc);
				afc = NULL;
				goto leave_cleanup;
			}
			FILE *f = NULL;
			uint64_t af = 0;
			/* local filename */
			char *localfile = NULL;
			if (asprintf(&localfile, "%s/%s.ipa", copy_path, appid) < 0) {
				fprintf(stderr, "Out of memory!?\n");
				goto leave_cleanup;
			}
			free(copy_path);

			f = fopen(localfile, "wb");
			if (!f) {
				fprintf(stderr, "ERROR: fopen: %s: %s\n", localfile, strerror(errno));
				free(localfile);
				goto leave_cleanup;
			}

			/* remote filename */
			char *remotefile = NULL;
			if (asprintf(&remotefile, "%s/%s.zip", APPARCH_PATH, appid) < 0) {
				fprintf(stderr, "Out of memory!?\n");
				goto leave_cleanup;
			}

			uint32_t fsize = 0;
			char **fileinfo = NULL;
			if ((afc_get_file_info(afc, remotefile, &fileinfo) != AFC_E_SUCCESS) || !fileinfo) {
				fprintf(stderr, "ERROR getting AFC file info for '%s' on device!\n", remotefile);
				fclose(f);
				free(remotefile);
				free(localfile);
				goto leave_cleanup;
			}

			int i;
			for (i = 0; fileinfo[i]; i+=2) {
				if (!strcmp(fileinfo[i], "st_size")) {
					fsize = atoi(fileinfo[i+1]);
					break;
				}
			}
			i = 0;
			while (fileinfo[i]) {
				free(fileinfo[i]);
				i++;
			}
			free(fileinfo);

			if (fsize == 0) {
				fprintf(stderr, "Hm... remote file length could not be determined. Cannot copy.\n");
				fclose(f);
				free(remotefile);
				free(localfile);
				goto leave_cleanup;
			}

			if ((afc_file_open(afc, remotefile, AFC_FOPEN_RDONLY, &af) != AFC_E_SUCCESS) || !af) {
				fclose(f);
				fprintf(stderr, "ERROR: could not open '%s' on device for reading!\n", remotefile);
				free(remotefile);
				free(localfile);
				goto leave_cleanup;
			}

			/* copy file over */
			printf("Copying '%s' --> '%s'\n", remotefile, localfile);
			free(remotefile);
			free(localfile);

			uint32_t amount = 0;
			uint32_t total = 0;
			char buf[8192];

			do {
				if (afc_file_read(afc, af, buf, sizeof(buf), &amount) != AFC_E_SUCCESS) {
					fprintf(stderr, "AFC Read error!\n");
					break;
				}

				if (amount > 0) {
					size_t written = fwrite(buf, 1, amount, f);
					if (written != amount) {
						fprintf(stderr, "Error when writing %d bytes to local file!\n", amount);
						break;
					}
					total += written;
				}
			} while (amount > 0);

			afc_file_close(afc, af);
			fclose(f);

			printf("done.\n");
			if (total != fsize) {
				fprintf(stderr, "WARNING: remote and local file sizes don't match (%d != %d)\n", fsize, total);
				if (remove_after_copy) {
					fprintf(stderr, "NOTE: archive file will NOT be removed from device\n");
					remove_after_copy = 0;
				}
			}

			if (remove_after_copy) {
				/* remove archive if requested */
				printf("Removing '%s'\n", appid);
				archive_mode = 0;
				remove_archive_mode = 1;
				free(options);
				options = NULL;
				if (LOCKDOWN_E_SUCCESS != lockdownd_client_new_with_handshake(phone, &client, "ideviceinstaller")) {
					fprintf(stderr, "Could not connect to lockdownd. Exiting.\n");
					goto leave_cleanup;
				}
				goto run_again;
			}
		}
		goto leave_cleanup;
	} else if (restore_mode) {
#ifdef HAVE_LIBIMOBILEDEVICE_1_1
		instproxy_restore(ipc, appid, NULL, status_cb, NULL);
#else
		instproxy_restore(ipc, appid, NULL, status_cb);
#endif
		wait_for_op_complete = 1;
		notification_expected = 1;
	} else if (remove_archive_mode) {
#ifdef HAVE_LIBIMOBILEDEVICE_1_1
		instproxy_remove_archive(ipc, appid, NULL, status_cb, NULL);
#else
		instproxy_remove_archive(ipc, appid, NULL, status_cb);
#endif
		wait_for_op_complete = 1;
	} else {
		printf
			("ERROR: no operation selected?! This should not be reached!\n");
		res = -2;
		goto leave_cleanup;
	}

	if (client) {
		/* not needed anymore */
		lockdownd_client_free(client);
		client = NULL;
	}

	do_wait_when_needed();

  leave_cleanup:
	if (np) {
		np_client_free(np);
	}
	if (ipc) {
		instproxy_client_free(ipc);
	}
	if (afc) {
		afc_client_free(afc);
	}
	if (client) {
		lockdownd_client_free(client);
	}
	idevice_free(phone);

	if (udid) {
		free(udid);
	}
	if (appid) {
		free(appid);
	}
	if (options) {
		free(options);
	}

	return res;
}
